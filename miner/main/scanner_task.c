/**
 * @file scanner_task.c
 * @brief Miner proxy handler — receives proxy_req via UART, streams
 *        chunks from ezShare back as JSON+base64 over UART.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_rom_crc.h"
#include "mbedtls/base64.h"
#include "scanner_task.h"
#include "uart_handler.h"
#include "wifi_manager.h"
#include "ezshare_client.h"
#include "nvs_config.h"
#include "config.h"
#include "o2ring_ble.h"
#include "ota_handler.h"

static const char *TAG = LOG_TAG_SCANNER;

static TaskHandle_t scanner_task_handle = NULL;
static bool task_running = false;
static scanner_state_t current_state = SCANNER_IDLE;

static char ez_ssid[33] = {0};
static char ez_pass[65] = {0};

static int64_t last_proxy_time_us = 0;

/* O2Ring BLE state */
static bool s_ble_initialized = false;
static int o2ring_req_id = 0;
static char o2ring_cmd[16] = {0};
static char o2ring_filename[O2RING_MAX_FILENAME] = {0};


/* Current proxy request */
static int proxy_req_id = 0;
static char proxy_path[512] = {0};
static uint32_t proxy_range_start = 0;
static uint32_t proxy_range_end = 0;

typedef struct {
    int req_id;
    uint16_t http_status;
    uint32_t content_length;
    uint32_t range_start;
    uint32_t range_end;
    bool meta_sent;
    bool error;
    bool aborted;   /* mule sent proxy_abort — client gone, stop streaming */
} proxy_ctx_t;

void scanner_task_load_ezshare_creds(void)
{
    if (nvs_config_has_ezshare()) {
        nvs_config_get_ezshare_ssid(ez_ssid, sizeof(ez_ssid));
        nvs_config_get_ezshare_pass(ez_pass, sizeof(ez_pass));
        ESP_LOGI(TAG, "NVS ezShare creds (SSID: %s)", ez_ssid);
    } else {
        strncpy(ez_ssid, EZSHARE_WIFI_SSID_DEFAULT, sizeof(ez_ssid) - 1);
        ez_ssid[sizeof(ez_ssid) - 1] = '\0';
        strncpy(ez_pass, EZSHARE_WIFI_PASSWORD_DEFAULT, sizeof(ez_pass) - 1);
        ez_pass[sizeof(ez_pass) - 1] = '\0';
        ESP_LOGW(TAG, "No NVS ezShare creds — using defaults (SSID: %s)", ez_ssid);
    }
}

/* ── UART JSON helpers ─────────────────────────────────────────── */

static void send_error_json(int req_id, const char *message, const char *code)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddStringToObject(root, "code", code);

    /* ezShare link diagnostics on every error. Without these a failure is just
     * "502" and the causes are indistinguishable from the mule's side:
     *   assoc=0 reason=201  card off / asleep / out of range (NO_AP_FOUND)
     *   assoc=0 reason=202  wrong ezShare password (AUTH_FAIL)
     *   assoc=1 rssi<=-80   associated but the link is too weak to move data
     * Cheap to attach, and it is the difference between a support answer and a
     * guess. The mule surfaces them on /api/status and in its logs. */
    cJSON_AddBoolToObject(root, "assoc", wifi_manager_is_connected());
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_rssi());
    cJSON_AddNumberToObject(root, "reason", wifi_manager_last_disc_reason());

    char *json = cJSON_PrintUnformatted(root);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(root);
}

static void send_proxy_meta(int req_id, uint16_t http_status,
                             uint32_t content_length, uint32_t total_size)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "proxy_meta");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON_AddNumberToObject(root, "st", http_status);
    cJSON_AddNumberToObject(root, "cl", content_length);
    cJSON_AddNumberToObject(root, "ts", total_size);
    char *json = cJSON_PrintUnformatted(root);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(root);
}

#define B64_BUF_SIZE ((FILE_CHUNK_SIZE * 4 / 3) + 8)
static char s_b64_buf[B64_BUF_SIZE];

static esp_err_t send_proxy_chunk(int req_id, size_t seq, bool is_last,
                                   const uint8_t *data, size_t data_len)
{
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, data, data_len);
    if (b64_len >= B64_BUF_SIZE) return ESP_ERR_INVALID_SIZE;

    size_t actual = 0;
    mbedtls_base64_encode((unsigned char *)s_b64_buf, b64_len, &actual, data, data_len);
    s_b64_buf[actual] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "proxy_chunk");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON_AddNumberToObject(root, "seq", seq);
    cJSON_AddBoolToObject(root, "last", is_last);
    /* CRC32 of the RAW bytes, so the mule can prove what it decoded is what we
     * read off the card. There is no integrity check anywhere below this: the
     * link has no parity, no checksum and no hardware flow control, and at
     * 921600 the realistic failure is a dropped byte under RX pressure rather
     * than a flipped bit. Base64 hides that — drop three bytes and the
     * remainder still decodes cleanly into plausible garbage. Cheap here:
     * esp_rom_crc32_le is a ROM routine.
     * Sent as a JSON number, so the mule must read valuedouble — a CRC above
     * 2^31 overflows cJSON's int field. */
    cJSON_AddNumberToObject(root, "c", (double)esp_rom_crc32_le(0, data, data_len));
    cJSON_AddStringToObject(root, "d", s_b64_buf);

    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (json) { err = uart_send_json(json); free(json); }

    cJSON_Delete(root);
    return err;
}

/* ── Chunk callback — sends meta + chunks over UART ────────────── */

static esp_err_t proxy_chunk_callback(const uint8_t *data, size_t len,
                                       size_t seq, bool is_last, void *ctx)
{
    proxy_ctx_t *pctx = (proxy_ctx_t *)ctx;

    /* Stop early if the mule aborted this stream (HTTP client disconnected).
     * Returning an error unwinds ezshare_raw_get_range so we don't keep pulling
     * the whole file off the card and flooding UART with chunks nobody reads. */
    if (uart_check_proxy_abort()) {
        ESP_LOGW(TAG, "proxy_abort received for req_id=%d — stopping stream", pctx->req_id);
        pctx->aborted = true;
        return ESP_FAIL;
    }

    if (!pctx->meta_sent) {
        uint32_t total_size;
        if (pctx->http_status == 206 && pctx->range_end == 0)
            total_size = pctx->range_start + pctx->content_length;
        else if (pctx->http_status == 206)
            total_size = 0;
        else
            total_size = pctx->content_length;

        send_proxy_meta(pctx->req_id, pctx->http_status,
                        pctx->content_length, total_size);
        pctx->meta_sent = true;
    }

    esp_err_t err = send_proxy_chunk(pctx->req_id, seq, is_last, data, len);
    if (err != ESP_OK) {
        pctx->error = true;
        return err;
    }

    ESP_LOGD(TAG, "chunk %zu sent (%zu bytes, last=%d)", seq, len, (int)is_last);
    return ESP_OK;
}

/* ── O2Ring JSON helpers ──────────────────────────────────────── */

static void send_o2ring_status(int req_id)
{
    const o2ring_device_info_t *info = o2ring_ble_get_info();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "o2ring_status");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON_AddBoolToObject(root, "connected", o2ring_ble_is_connected());
    cJSON_AddStringToObject(root, "model", info && info->valid ? info->model : "");
    cJSON_AddStringToObject(root, "serial", info && info->valid ? info->serial : "");
    cJSON_AddNumberToObject(root, "battery", info && info->valid ? info->battery : 0);
    cJSON_AddNumberToObject(root, "file_count", info && info->valid ? info->file_count : 0);
    char *json = cJSON_PrintUnformatted(root);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(root);
}

static void send_o2ring_files(int req_id)
{
    const o2ring_device_info_t *info = o2ring_ble_get_info();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "o2ring_files");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON *arr = cJSON_AddArrayToObject(root, "files");
    if (info && info->valid) {
        for (int i = 0; i < info->file_count; i++) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(info->files[i].name));
        }
        cJSON_AddNumberToObject(root, "battery", info->battery);
    } else {
        cJSON_AddNumberToObject(root, "battery", 0);
    }
    char *json = cJSON_PrintUnformatted(root);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(root);
}

static void send_o2ring_live(int req_id)
{
    const o2ring_live_t *live = o2ring_ble_get_live();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "o2ring_live");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON_AddNumberToObject(root, "spo2", live ? live->spo2 : 0);
    cJSON_AddNumberToObject(root, "hr", live ? live->hr : 0);
    cJSON_AddNumberToObject(root, "motion", live ? live->motion : 0);
    cJSON_AddNumberToObject(root, "vibration", live ? live->vibration : 0);
    cJSON_AddBoolToObject(root, "valid", live ? live->valid : false);
    char *json = cJSON_PrintUnformatted(root);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(root);
}

/* ── BLE management ──────────────────────────────────────────── */

static bool ensure_ble_ready(int req_id)
{
    /* BLE gate: off by default. Bringing up BLE drops the ezShare WiFi link
     * (shared radio on the C3), interrupting CPAP collection — so don't touch the
     * radio at all unless explicitly enabled via NVS miner/ble_active=1. */
    if (!nvs_config_ble_active()) {
        ESP_LOGW(TAG, "O2Ring request but ble_active=0 — BLE disabled");
        send_error_json(req_id, "O2Ring BLE disabled", "BLE_DISABLED");
        return false;
    }

    /* Disconnect WiFi if connected (radio shared on ESP32-C3) */
    if (wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "Disconnecting WiFi for BLE");
        wifi_manager_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!s_ble_initialized) {
        esp_err_t ret = o2ring_ble_init();
        if (ret != ESP_OK) {
            send_error_json(req_id, "BLE init failed", "BLE_INIT_FAIL");
            return false;
        }
        s_ble_initialized = true;
    }

    if (!o2ring_ble_is_connected()) {
        o2ring_ble_set_auto_reconnect(true);
        esp_err_t ret = o2ring_ble_connect_and_wait(O2RING_CONNECT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            send_error_json(req_id, "O2Ring not found", "BLE_NOT_FOUND");
            return false;
        }
    }
    return true;
}

static void release_ble(void)
{
    if (!s_ble_initialized) return;
    o2ring_ble_stop();
    o2ring_ble_deinit();
    s_ble_initialized = false;
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ── WiFi management with idle timeout ─────────────────────────── */

static bool ensure_ezshare_connected(void)
{
    if (wifi_manager_is_connected()) return true;

    ESP_LOGI(TAG, "Connecting to ezShare (%s)...", ez_ssid);
    if (wifi_manager_connect(ez_ssid, ez_pass, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK) {
        ezshare_client_init();
        return true;
    }
    ESP_LOGE(TAG, "ezShare WiFi connect failed");
    return false;
}

static void check_idle_disconnect(void)
{
    if (!wifi_manager_is_connected()) return;
    if (last_proxy_time_us == 0) return;

    int64_t elapsed_ms = (esp_timer_get_time() - last_proxy_time_us) / 1000;
    if (elapsed_ms > PROXY_IDLE_TIMEOUT_MS) {
        ESP_LOGI(TAG, "Idle timeout — disconnecting from ezShare");
        wifi_manager_disconnect();
        last_proxy_time_us = 0;
    }
}

/* ── Handle set_config from mule ───────────────────────────────── */

static void handle_set_config(cJSON *root)
{
    cJSON *ssid_j = cJSON_GetObjectItem(root, "ez_ssid");
    cJSON *pass_j = cJSON_GetObjectItem(root, "ez_pass");

    if (!ssid_j || !cJSON_IsString(ssid_j)) return;
    const char *new_pass = (pass_j && cJSON_IsString(pass_j)) ? pass_j->valuestring : "";

    /* The mule sends this on every one of its boots. Restarting unconditionally
     * meant a mule reboot always dragged the miner down with it — including
     * mid-transfer, if the mule had crashed and come back. Only restart when
     * the credentials actually changed; a restart is how they take effect, so
     * an unchanged config needs nothing. */
    char cur_ssid[33] = {0}, cur_pass[65] = {0};
    nvs_config_get_ezshare_ssid(cur_ssid, sizeof(cur_ssid));
    nvs_config_get_ezshare_pass(cur_pass, sizeof(cur_pass));
    bool changed = strcmp(cur_ssid, ssid_j->valuestring) != 0 ||
                   strcmp(cur_pass, new_pass) != 0;

    if (changed) {
        nvs_config_set_ezshare(ssid_j->valuestring, new_pass);
        ESP_LOGI(TAG, "set_config: SSID=%s (changed — restarting to apply)",
                 ssid_j->valuestring);
    } else {
        ESP_LOGI(TAG, "set_config: SSID=%s (unchanged — staying up)",
                 ssid_j->valuestring);
    }

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "type", "config_ack");
    cJSON_AddStringToObject(ack, "status", "ok");
    /* The mule waits on this to know whether the miner is about to disappear. */
    cJSON_AddBoolToObject(ack, "changed", changed);
    char *json = cJSON_PrintUnformatted(ack);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(ack);

    if (changed) {
        uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(2000));
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

/* ── O2Ring streaming download ─────────────────────────────────── */

typedef struct {
    int    req_id;
    size_t seq;
    bool   failed;
} o2_dl_ctx_t;

/* One BLE block, straight onto the link. Returning false unwinds the download
 * inside o2ring_ble, so a link that has stopped accepting frames stops the
 * BLE read too rather than pulling the rest of a file nobody is reading. */
static bool o2_dl_chunk_cb(const uint8_t *data, size_t len, uint32_t offset, void *ctx)
{
    o2_dl_ctx_t *c = (o2_dl_ctx_t *)ctx;
    if (send_proxy_chunk(c->req_id, c->seq, false, data, len) != ESP_OK) {
        c->failed = true;
        return false;
    }
    c->seq++;
    return true;
}

/* ── Control messages from the mule ────────────────────────────── */

/* Send a small {"type":..., ...} frame with one extra field. */
static void send_simple(const char *type, const char *key, cJSON *value)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { if (value) cJSON_Delete(value); return; }
    cJSON_AddStringToObject(root, "type", type);
    if (key && value) cJSON_AddItemToObject(root, key, value);
    char *json = cJSON_PrintUnformatted(root);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(root);
}

/* Reboot after giving the reply time to reach the wire. uart_send_json already
 * waits for the TX FIFO, but the mule still has to read and act on it, and a
 * restart mid-flush would leave it waiting out a full timeout for a reply that
 * was already gone. */
static void reply_then_restart(const char *what)
{
    ESP_LOGW(TAG, "%s requested by mule — restarting", what);
    uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(2000));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void handle_version_req(void)
{
    send_simple("version_resp", "ver", cJSON_CreateString(FW_VERSION));
    ESP_LOGI(TAG, "version_req -> %s", FW_VERSION);
}

/* `reset` wipes config; `reboot` deliberately does not. Keeping them distinct
 * is the whole point — a reboot is a routine recovery action, while a reset
 * makes the miner forget the ezShare card. */
static void handle_reset(void)
{
    nvs_config_erase_all();
    send_simple("ack", "of", cJSON_CreateString("reset"));
    reply_then_restart("reset");
}

static void handle_reboot(void)
{
    send_simple("ack", "of", cJSON_CreateString("reboot"));
    reply_then_restart("reboot");
}

/* ── OTA ──────────────────────────────────────────────────────────
 *
 * Entering SCANNER_OTA is the miner's half of the update gate: while an image
 * is being written, the dispatcher below accepts ota_chunk and ota_finish and
 * nothing else. The mule refuses conflicting HTTP requests at its own edge,
 * but that only covers requests arriving over HTTP; this covers the wire.
 */

/* Decoded chunk payload. static, not stack: the scanner task has 8 KB and
 * these two buffers are 9 KB together. Safe because only this task touches
 * them, and only while an OTA is open. */
static uint8_t s_ota_bin[FILE_CHUNK_SIZE];
static char    s_ota_b64[B64_BUF_SIZE];

static void send_ack(const char *of)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "of", of);
    char *json = cJSON_PrintUnformatted(root);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(root);
}

static void handle_ota_begin(cJSON *root)
{
    cJSON *tot = cJSON_GetObjectItem(root, "total");
    if (!cJSON_IsNumber(tot) || tot->valuedouble <= 0) {
        send_error_json(0, "ota_begin without a size", "OTA_BAD_ARG");
        return;
    }
    esp_err_t err = miner_ota_begin((uint32_t)tot->valuedouble);
    if (err != ESP_OK) {
        send_error_json(0, esp_err_to_name(err), "OTA_BEGIN_FAILED");
        return;
    }
    current_state = SCANNER_OTA;
    send_ack("ota_begin");
}

/* Returns false when the transfer has ended, either way. */
static bool handle_ota_chunk(cJSON *root)
{
    cJSON *off_j = cJSON_GetObjectItem(root, "off");
    cJSON *d_j   = cJSON_GetObjectItem(root, "d");
    cJSON *crc_j = cJSON_GetObjectItem(root, "c");
    if (!cJSON_IsNumber(off_j) || !cJSON_IsString(d_j)) {
        send_error_json(0, "malformed ota_chunk", "OTA_BAD_CHUNK");
        miner_ota_abort();
        return false;
    }

    size_t b64_len = strlen(d_j->valuestring);
    if (b64_len >= sizeof(s_ota_b64)) {
        send_error_json(0, "ota_chunk too large", "OTA_BAD_CHUNK");
        miner_ota_abort();
        return false;
    }
    memcpy(s_ota_b64, d_j->valuestring, b64_len);

    size_t bin_len = 0;
    if (mbedtls_base64_decode(s_ota_bin, sizeof(s_ota_bin), &bin_len,
                              (const unsigned char *)s_ota_b64, b64_len) != 0) {
        send_error_json(0, "ota_chunk failed to decode", "OTA_BAD_CHUNK");
        miner_ota_abort();
        return false;
    }

    /* Verify before writing. Flashing a corrupt chunk and finding out at
     * esp_ota_end means the whole transfer is wasted; worse, a corruption that
     * still passes the image hash would be written permanently. */
    if (cJSON_IsNumber(crc_j)) {
        uint32_t want = (uint32_t)crc_j->valuedouble;
        uint32_t got  = esp_rom_crc32_le(0, s_ota_bin, bin_len);
        if (got != want) {
            ESP_LOGE(TAG, "ota_chunk CRC mismatch at offset %lu",
                     (unsigned long)off_j->valuedouble);
            send_error_json(0, "ota_chunk CRC mismatch", "OTA_CRC");
            miner_ota_abort();
            return false;
        }
    }

    if (miner_ota_write((uint32_t)off_j->valuedouble, s_ota_bin, bin_len) != ESP_OK) {
        send_error_json(0, "ota_chunk write failed", "OTA_WRITE");
        return false;                    /* miner_ota_write already aborted */
    }
    send_ack("ota_chunk");
    return true;
}

static void handle_ota_finish(void)
{
    esp_err_t err = miner_ota_finish();
    if (err != ESP_OK) {
        send_error_json(0, esp_err_to_name(err), "OTA_FINISH_FAILED");
        current_state = SCANNER_IDLE;
        return;
    }
    send_ack("ota_finish");
    reply_then_restart("ota_finish");
}

static void handle_o2_state_req(void)
{
    send_simple("o2_state_resp", "enabled", cJSON_CreateBool(nvs_config_ble_active()));
}

static void handle_o2_set_enabled(cJSON *root)
{
    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(en)) {
        ESP_LOGW(TAG, "o2_set_enabled without a boolean 'enabled' — ignoring");
        return;
    }
    bool want = cJSON_IsTrue(en);
    if (want == nvs_config_ble_active()) {
        /* Already in the requested state. Answer and stay up rather than
         * dropping the data link for a no-op. */
        ESP_LOGI(TAG, "o2_set_enabled=%d already set — not restarting", want);
        handle_o2_state_req();
        return;
    }
    nvs_config_set_ble_active(want);
    send_simple("o2_state_resp", "enabled", cJSON_CreateBool(want));
    reply_then_restart("o2_set_enabled");
}

/* ── Main loop ─────────────────────────────────────────────────── */

static void scanner_task_loop(void *pvParameters)
{
    char uart_buf[JSON_BUFFER_SIZE];

    while (task_running) {
        switch (current_state) {
            case SCANNER_IDLE: {
                check_idle_disconnect();
                /* Roll back if a freshly written image has run out of time to
                 * show it can still hear the mule. */
                miner_ota_check_rollback_deadline();

                int len = uart_receive_json(uart_buf, sizeof(uart_buf), 1000);
                if (len <= 0) {
                    vTaskDelay(pdMS_TO_TICKS(SCANNER_POLL_INTERVAL_MS));
                    break;
                }

                cJSON *root = cJSON_Parse(uart_buf);
                if (!root) break;

                /* A frame that parsed is proof the link works, which is what a
                 * pending image needs to confirm itself. Noise must never
                 * count, so this sits after the parse, not after the read. */
                miner_ota_confirm_boot();

                cJSON *type = cJSON_GetObjectItem(root, "type");
                if (!type || !cJSON_IsString(type)) { cJSON_Delete(root); break; }

                if (strcmp(type->valuestring, "set_config") == 0) {
                    handle_set_config(root);
                } else if (strcmp(type->valuestring, "version_req") == 0) {
                    handle_version_req();
                } else if (strcmp(type->valuestring, "reboot") == 0) {
                    handle_reboot();
                } else if (strcmp(type->valuestring, "reset") == 0) {
                    handle_reset();
                } else if (strcmp(type->valuestring, "o2_state_req") == 0) {
                    handle_o2_state_req();
                } else if (strcmp(type->valuestring, "o2_set_enabled") == 0) {
                    handle_o2_set_enabled(root);
                } else if (strcmp(type->valuestring, "ota_begin") == 0) {
                    handle_ota_begin(root);
                } else if (strcmp(type->valuestring, "o2ring_req") == 0) {
                    cJSON *id_j  = cJSON_GetObjectItem(root, "id");
                    cJSON *cmd_j = cJSON_GetObjectItem(root, "cmd");
                    cJSON *name_j = cJSON_GetObjectItem(root, "name");

                    if (id_j && cmd_j && cJSON_IsString(cmd_j)) {
                        o2ring_req_id = id_j->valueint;
                        strncpy(o2ring_cmd, cmd_j->valuestring, sizeof(o2ring_cmd) - 1);
                        o2ring_cmd[sizeof(o2ring_cmd) - 1] = '\0';
                        if (name_j && cJSON_IsString(name_j)) {
                            strncpy(o2ring_filename, name_j->valuestring, sizeof(o2ring_filename) - 1);
                            o2ring_filename[sizeof(o2ring_filename) - 1] = '\0';
                        } else {
                            o2ring_filename[0] = '\0';
                        }

                        ESP_LOGI(TAG, "o2ring_req id=%d cmd=%s name=%s",
                                 o2ring_req_id, o2ring_cmd, o2ring_filename);
                        current_state = SCANNER_O2RING;
                    }
                } else if (strcmp(type->valuestring, "proxy_req") == 0) {
                    cJSON *id_j   = cJSON_GetObjectItem(root, "id");
                    cJSON *path_j = cJSON_GetObjectItem(root, "path");
                    cJSON *rs_j   = cJSON_GetObjectItem(root, "rs");
                    cJSON *re_j   = cJSON_GetObjectItem(root, "re");

                    if (id_j && path_j && cJSON_IsString(path_j)) {
                        proxy_req_id = id_j->valueint;
                        strncpy(proxy_path, path_j->valuestring, sizeof(proxy_path) - 1);
                        proxy_path[sizeof(proxy_path) - 1] = '\0';
                        proxy_range_start = rs_j ? (uint32_t)rs_j->valueint : 0;
                        proxy_range_end   = re_j ? (uint32_t)re_j->valueint : 0;

                        ESP_LOGI(TAG, "proxy_req id=%d path=%s range=%lu-%lu",
                                 proxy_req_id, proxy_path,
                                 (unsigned long)proxy_range_start,
                                 (unsigned long)proxy_range_end);
                        current_state = SCANNER_PROXY;
                    }
                }

                cJSON_Delete(root);
                break;
            }

            case SCANNER_PROXY: {
                release_ble();  /* Free BLE heap before WiFi */

                if (!ensure_ezshare_connected()) {
                    send_error_json(proxy_req_id, "ezShare unreachable", "WIFI_FAILED");
                    current_state = SCANNER_ERROR;
                    break;
                }

                proxy_ctx_t pctx = {
                    .req_id = proxy_req_id,
                    .http_status = 0,
                    .content_length = 0,
                    .range_start = proxy_range_start,
                    .range_end = proxy_range_end,
                    .meta_sent = false,
                    .error = false,
                };

                esp_err_t err = ezshare_raw_get_range(
                    proxy_path, FILE_CHUNK_SIZE,
                    proxy_range_start, proxy_range_end,
                    &pctx.http_status, &pctx.content_length,
                    proxy_chunk_callback, &pctx);

                if (pctx.aborted) {
                    /* Client gone — mule already abandoned the response. Stay
                     * silent and let the channel go idle (mule drains any
                     * already-queued chunks). */
                    ESP_LOGW(TAG, "proxy req_id=%d aborted by mule", proxy_req_id);
                } else if (err != ESP_OK || pctx.error) {
                    ESP_LOGE(TAG, "proxy failed: %s", esp_err_to_name(err));
                    if (!pctx.meta_sent)
                        send_error_json(proxy_req_id, "ezShare request failed", "HTTP_FAILED");
                }

                last_proxy_time_us = esp_timer_get_time();
                current_state = SCANNER_IDLE;
                break;
            }

            case SCANNER_O2RING: {
                if (strcmp(o2ring_cmd, "status") == 0) {
                    /* Status returns cached state — no connection needed */
                    send_o2ring_status(o2ring_req_id);
                } else if (strcmp(o2ring_cmd, "files") == 0) {
                    if (!ensure_ble_ready(o2ring_req_id)) {
                        current_state = SCANNER_IDLE;
                        break;
                    }
                    esp_err_t ret = o2ring_ble_refresh_info();
                    if (ret != ESP_OK) {
                        send_error_json(o2ring_req_id, "Failed to read file list", "BLE_READ_FAIL");
                    } else {
                        send_o2ring_files(o2ring_req_id);
                    }
                } else if (strcmp(o2ring_cmd, "live") == 0) {
                    if (!ensure_ble_ready(o2ring_req_id)) {
                        current_state = SCANNER_IDLE;
                        break;
                    }
                    esp_err_t ret = o2ring_ble_read_sensors();
                    if (ret != ESP_OK) {
                        send_error_json(o2ring_req_id, "Failed to read sensors", "BLE_READ_FAIL");
                    } else {
                        send_o2ring_live(o2ring_req_id);
                    }
                } else if (strcmp(o2ring_cmd, "download") == 0) {
                    if (!ensure_ble_ready(o2ring_req_id)) {
                        current_state = SCANNER_IDLE;
                        break;
                    }
                    /* Refresh info to ensure file list is current */
                    o2ring_ble_refresh_info();

                    /* Stream each BLE block straight onto the link instead of
                     * assembling the whole file first. The old path malloc'd a
                     * 48 KB buffer on a board whose free heap is measured in
                     * tens of KB, and capped downloads at that size for files
                     * the README itself says run 30-50 KB. Nothing is buffered
                     * now, so the ceiling is gone and so is the allocation.
                     *
                     * The size is not known until the transfer ends, so the
                     * meta goes out with length 0 (the mule streams chunked
                     * and ignores it here) and a final empty chunk carries the
                     * end marker. */
                    send_proxy_meta(o2ring_req_id, 200, 0, 0);

                    o2_dl_ctx_t dctx = { .req_id = o2ring_req_id, .seq = 0, .failed = false };
                    size_t out_len = 0;
                    esp_err_t ret = o2ring_ble_download_file_stream(
                        o2ring_filename, o2_dl_chunk_cb, &dctx, &out_len);

                    if (ret != ESP_OK || dctx.failed || out_len == 0) {
                        /* Chunks may already be on the wire, so the mule has to
                         * learn this failed rather than wait for an end marker
                         * that is never coming. */
                        ESP_LOGE(TAG, "O2Ring download failed after %u chunk(s)",
                                 (unsigned)dctx.seq);
                        send_error_json(o2ring_req_id, "File download failed", "BLE_READ_FAIL");
                        current_state = SCANNER_IDLE;
                        break;
                    }

                    static const uint8_t empty = 0;
                    send_proxy_chunk(o2ring_req_id, dctx.seq, true, &empty, 0);
                    ESP_LOGI(TAG, "O2Ring download complete: %u B in %u chunk(s)",
                             (unsigned)out_len, (unsigned)dctx.seq);
                } else {
                    send_error_json(o2ring_req_id, "Unknown o2ring command", "INVALID_CMD");
                }
                current_state = SCANNER_IDLE;
                break;
            }

            case SCANNER_OTA: {
                /* The gate: while an image is being written, only OTA frames
                 * are honoured. Anything else is refused rather than ignored,
                 * so a client that asks for a file mid-update gets an error it
                 * can act on instead of a silence it has to time out.
                 *
                 * The timeout is generous because the mule pauses between
                 * chunks to read its own source (HTTP body or network), but it
                 * is not unbounded: a mule that dies mid-transfer must not
                 * leave the miner stuck here forever. */
                int len = uart_receive_json(uart_buf, sizeof(uart_buf), 30000);
                if (len <= 0) {
                    ESP_LOGE(TAG, "OTA: no frame from the mule — abandoning");
                    miner_ota_abort();
                    current_state = SCANNER_IDLE;
                    break;
                }

                cJSON *root = cJSON_Parse(uart_buf);
                if (!root) break;
                cJSON *type = cJSON_GetObjectItem(root, "type");
                if (!type || !cJSON_IsString(type)) { cJSON_Delete(root); break; }

                if (strcmp(type->valuestring, "ota_chunk") == 0) {
                    if (!handle_ota_chunk(root)) current_state = SCANNER_IDLE;
                } else if (strcmp(type->valuestring, "ota_finish") == 0) {
                    handle_ota_finish();      /* restarts on success */
                } else if (strcmp(type->valuestring, "ota_abort") == 0) {
                    ESP_LOGW(TAG, "OTA abandoned by the mule");
                    miner_ota_abort();
                    send_ack("ota_abort");
                    current_state = SCANNER_IDLE;
                } else {
                    ESP_LOGW(TAG, "'%s' refused: a firmware update is in progress",
                             type->valuestring);
                    send_error_json(0, "firmware update in progress", "OTA_BUSY");
                }
                cJSON_Delete(root);
                break;
            }

            case SCANNER_ERROR:
                vTaskDelay(pdMS_TO_TICKS(SCANNER_RETRY_DELAY_MS));
                current_state = SCANNER_IDLE;
                break;
        }
    }
    vTaskDelete(NULL);
}

esp_err_t scanner_task_init(void)
{
    current_state = SCANNER_IDLE;
    task_running = false;
    scanner_task_load_ezshare_creds();
    return ESP_OK;
}

esp_err_t scanner_task_start(void)
{
    if (task_running) return ESP_OK;
    task_running = true;
    if (xTaskCreate(scanner_task_loop, "miner_task", SCANNER_TASK_STACK_SIZE,
                    NULL, SCANNER_TASK_PRIORITY, &scanner_task_handle) != pdPASS) {
        task_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void scanner_task_stop(void) { task_running = false; }
scanner_state_t scanner_task_get_state(void) { return current_state; }
