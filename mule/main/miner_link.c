/**
 * @file miner_link.c
 * @brief Mule-side control exchanges with the miner (see miner_link.h).
 */

#include "miner_link.h"
#include "uart_handler.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "miner_link";

/* How long to wait for a reply to a small control frame. These are answered
 * from RAM on the miner, so they are fast — but the miner may be mid-chunk on
 * an ezShare read when the request lands, and it only dispatches control
 * messages from its idle state. */
#define CTRL_REPLY_TIMEOUT_MS   3000
/* Acquiring the link. Kept short: these run on the single httpd task, and a
 * status page that hangs for the length of someone's download is worse than
 * one that reports the miner as briefly unreachable. */
#define CTRL_LOCK_TIMEOUT_MS    2000

static char s_miner_version[32] = {0};
static miner_ezshare_diag_t s_diag;

/* ── helpers ──────────────────────────────────────────────────────── */

static esp_err_t send_simple(const char *type, const char *key, cJSON *value)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { if (value) cJSON_Delete(value); return ESP_ERR_NO_MEM; }
    cJSON_AddStringToObject(root, "type", type);
    if (key && value) cJSON_AddItemToObject(root, key, value);

    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (json) { err = uart_send_json(json); free(json); }
    cJSON_Delete(root);
    return err;
}

/**
 * Wait for a frame of the given type, discarding anything else.
 *
 * Discarding matters: a control request can land while the tail of a previous
 * transfer is still draining, and taking the first frame that arrives would
 * mean answering "what version is the miner" with a chunk of an EDF. Caller
 * owns the returned object.
 */
static cJSON *await_reply(const char *want_type, uint32_t timeout_ms)
{
    static char buf[1024];
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        int len = uart_receive_json(buf, sizeof(buf), 300);
        if (len <= 0) continue;

        cJSON *msg = cJSON_Parse(buf);
        if (!msg) continue;

        cJSON *type = cJSON_GetObjectItem(msg, "type");
        if (cJSON_IsString(type) && strcmp(type->valuestring, want_type) == 0)
            return msg;

        cJSON_Delete(msg);
    }
    return NULL;
}

/* Send one frame and wait for one reply, holding the link across both. */
static cJSON *exchange(const char *send_type, const char *key, cJSON *value,
                       const char *want_type, uint32_t timeout_ms)
{
    if (!uart_link_lock(CTRL_LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "%s: link busy", send_type);
        if (value) cJSON_Delete(value);
        return NULL;
    }

    cJSON *reply = NULL;
    if (send_simple(send_type, key, value) == ESP_OK)
        reply = await_reply(want_type, timeout_ms);
    else
        ESP_LOGE(TAG, "%s: send failed", send_type);

    uart_link_unlock();
    return reply;
}

/* ── version ──────────────────────────────────────────────────────── */

bool miner_link_query_version(char *out, size_t out_size)
{
    cJSON *reply = exchange("version_req", NULL, NULL,
                            "version_resp", CTRL_REPLY_TIMEOUT_MS);
    if (!reply) {
        ESP_LOGW(TAG, "miner did not answer version_req");
        return false;
    }

    bool ok = false;
    cJSON *ver = cJSON_GetObjectItem(reply, "ver");
    if (cJSON_IsString(ver) && ver->valuestring[0]) {
        strncpy(s_miner_version, ver->valuestring, sizeof(s_miner_version) - 1);
        s_miner_version[sizeof(s_miner_version) - 1] = '\0';
        if (out && out_size) {
            strncpy(out, s_miner_version, out_size - 1);
            out[out_size - 1] = '\0';
        }
        ESP_LOGI(TAG, "miner firmware %s", s_miner_version);
        ok = true;
    }
    cJSON_Delete(reply);
    return ok;
}

const char *miner_link_cached_version(void)
{
    return s_miner_version[0] ? s_miner_version : "unknown";
}

/* ── reboot / reset ───────────────────────────────────────────────── */

/* The miner acks, then restarts. A missing ack is not proof the command was
 * ignored — it may have restarted before the ack drained — so these report
 * whether the ack arrived, and callers should treat that as advisory. */
bool miner_link_reboot(void)
{
    cJSON *reply = exchange("reboot", NULL, NULL, "ack", CTRL_REPLY_TIMEOUT_MS);
    if (reply) { cJSON_Delete(reply); return true; }
    ESP_LOGW(TAG, "no ack for reboot (miner may have restarted first)");
    return false;
}

bool miner_link_reset(void)
{
    cJSON *reply = exchange("reset", NULL, NULL, "ack", CTRL_REPLY_TIMEOUT_MS);
    if (reply) { cJSON_Delete(reply); return true; }
    ESP_LOGW(TAG, "no ack for reset (miner may have restarted first)");
    return false;
}

/* ── O2Ring gate ──────────────────────────────────────────────────── */

static bool read_enabled(cJSON *reply, bool *enabled)
{
    if (!reply) return false;
    bool ok = false;
    cJSON *en = cJSON_GetObjectItem(reply, "enabled");
    if (cJSON_IsBool(en)) { *enabled = cJSON_IsTrue(en); ok = true; }
    cJSON_Delete(reply);
    return ok;
}

bool miner_link_get_o2_enabled(bool *enabled)
{
    if (!enabled) return false;
    return read_enabled(exchange("o2_state_req", NULL, NULL,
                                 "o2_state_resp", CTRL_REPLY_TIMEOUT_MS), enabled);
}

bool miner_link_set_o2_enabled(bool enabled)
{
    bool got = false;
    if (!read_enabled(exchange("o2_set_enabled", "enabled", cJSON_CreateBool(enabled),
                               "o2_state_resp", CTRL_REPLY_TIMEOUT_MS), &got))
        return false;
    return got == enabled;
}

/* ── ezShare diagnostics ──────────────────────────────────────────── */

void miner_link_note_diag(bool assoc, int rssi, int reason)
{
    s_diag.valid   = true;
    s_diag.assoc   = assoc;
    s_diag.rssi    = rssi;
    s_diag.reason  = reason;
    s_diag.when_us = esp_timer_get_time();

    /* Log the interpretation, not just the numbers — these two codes cover
     * most real ezShare failures and mean completely different things. */
    if (!assoc && reason == 201)
        ESP_LOGW(TAG, "ezShare: AP not found (card off, asleep, or out of range)");
    else if (!assoc && reason == 202)
        ESP_LOGW(TAG, "ezShare: authentication failed (wrong card password)");
    else if (assoc && rssi != 0 && rssi <= -80)
        ESP_LOGW(TAG, "ezShare: associated but weak (%d dBm)", rssi);
}

void miner_link_get_diag(miner_ezshare_diag_t *out)
{
    if (out) *out = s_diag;
}
