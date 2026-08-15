/**
 * @file ota_miner.c
 * @brief Streaming a firmware image to the miner (see ota_miner.h).
 */

#include "ota_miner.h"
#include "uart_handler.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota_miner";

/* Raw bytes per chunk. Matches the proxy chunk size, so the base64 form and
 * the JSON envelope stay inside UART_LINE_MAX on both boards. */
#define OTA_CHUNK_RAW       PROXY_CHUNK_SIZE
#define OTA_B64_SIZE        ((OTA_CHUNK_RAW * 4 / 3) + 8)

/* Per-chunk acknowledgement wait. The miner writes to flash between chunks,
 * and an erase on a busy page is not instant. */
#define OTA_ACK_TIMEOUT_MS      10000
/* ota_begin erases the destination partition before answering, which is the
 * slowest step in the whole exchange. */
#define OTA_BEGIN_TIMEOUT_MS    30000
/* ota_finish validates the image, including its SHA256, before answering. */
#define OTA_FINISH_TIMEOUT_MS   20000
/* Acquiring the link. Long: an update is a deliberate act and worth waiting
 * for an in-flight transfer to finish, unlike a status poll. */
#define OTA_LOCK_TIMEOUT_MS     15000

static bool     s_open;
static uint32_t s_written;
static char     s_b64[OTA_B64_SIZE];

/* ── frame helpers ────────────────────────────────────────────────── */

/* Wait for an ack, treating an error frame as a definite failure rather than
 * waiting out the timeout. Returns ESP_OK only on a real ack. */
static esp_err_t await_ack(const char *what, uint32_t timeout_ms)
{
    static char buf[512];
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        int len = uart_receive_json(buf, sizeof(buf), 300);
        if (len <= 0) continue;

        cJSON *msg = cJSON_Parse(buf);
        if (!msg) continue;

        cJSON *type = cJSON_GetObjectItem(msg, "type");
        esp_err_t rc = ESP_ERR_NOT_FOUND;
        if (cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "ack") == 0) {
                rc = ESP_OK;
            } else if (strcmp(type->valuestring, "error") == 0) {
                cJSON *m = cJSON_GetObjectItem(msg, "message");
                ESP_LOGE(TAG, "%s refused: %s", what,
                         cJSON_IsString(m) ? m->valuestring : "unknown");
                rc = ESP_FAIL;
            }
        }
        cJSON_Delete(msg);
        if (rc != ESP_ERR_NOT_FOUND) return rc;
        /* Anything else is a stale frame from before this transfer; ignore. */
    }
    ESP_LOGE(TAG, "%s: no reply from the miner", what);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_frame(cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (json) { err = uart_send_json(json); free(json); }
    cJSON_Delete(root);
    return err;
}

/* ── transfer ─────────────────────────────────────────────────────── */

esp_err_t ota_miner_begin(uint32_t total_size)
{
    if (s_open) return ESP_ERR_INVALID_STATE;

    if (!uart_link_lock(OTA_LOCK_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "link busy — not starting an update");
        return ESP_ERR_TIMEOUT;
    }

    /* Anything left over from an earlier request would be read as this
     * transfer's first acknowledgement. */
    uart_rx_flush();

    cJSON *root = cJSON_CreateObject();
    if (!root) { uart_link_unlock(); return ESP_ERR_NO_MEM; }
    cJSON_AddStringToObject(root, "type", "ota_begin");
    cJSON_AddNumberToObject(root, "total", (double)total_size);

    esp_err_t err = send_frame(root);
    if (err == ESP_OK) err = await_ack("ota_begin", OTA_BEGIN_TIMEOUT_MS);

    if (err != ESP_OK) {
        uart_link_unlock();
        return err;
    }

    s_open    = true;
    s_written = 0;
    ESP_LOGW(TAG, "miner update started (%lu B)", (unsigned long)total_size);
    return ESP_OK;
}

esp_err_t ota_miner_write(const uint8_t *data, size_t len)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;

    while (len > 0) {
        size_t n = len > OTA_CHUNK_RAW ? OTA_CHUNK_RAW : len;

        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len, data, n);
        if (b64_len >= sizeof(s_b64)) return ESP_ERR_INVALID_SIZE;

        size_t actual = 0;
        mbedtls_base64_encode((unsigned char *)s_b64, sizeof(s_b64), &actual, data, n);
        s_b64[actual] = '\0';

        cJSON *root = cJSON_CreateObject();
        if (!root) return ESP_ERR_NO_MEM;
        cJSON_AddStringToObject(root, "type", "ota_chunk");
        /* The miner checks this against its own count, so a dropped or
         * duplicated chunk fails the transfer instead of silently writing a
         * misaligned image. */
        cJSON_AddNumberToObject(root, "off", (double)s_written);
        cJSON_AddNumberToObject(root, "c", (double)esp_rom_crc32_le(0, data, n));
        cJSON_AddStringToObject(root, "d", s_b64);

        esp_err_t err = send_frame(root);
        if (err == ESP_OK) err = await_ack("ota_chunk", OTA_ACK_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "update failed at offset %lu", (unsigned long)s_written);
            return err;
        }

        s_written += n;
        data      += n;
        len       -= n;
    }
    return ESP_OK;
}

esp_err_t ota_miner_end(bool commit)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;

    cJSON *root = cJSON_CreateObject();
    esp_err_t err = ESP_ERR_NO_MEM;
    if (root) {
        cJSON_AddStringToObject(root, "type", commit ? "ota_finish" : "ota_abort");
        err = send_frame(root);
        if (err == ESP_OK)
            err = await_ack(commit ? "ota_finish" : "ota_abort",
                            commit ? OTA_FINISH_TIMEOUT_MS : OTA_ACK_TIMEOUT_MS);
    }

    s_open = false;
    uart_link_unlock();

    if (commit && err == ESP_OK)
        ESP_LOGW(TAG, "miner accepted %lu B and is restarting", (unsigned long)s_written);
    return err;
}

uint32_t ota_miner_written(void) { return s_written; }
