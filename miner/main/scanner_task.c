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
#include "mbedtls/base64.h"
#include "scanner_task.h"
#include "uart_handler.h"
#include "wifi_manager.h"
#include "ezshare_client.h"
#include "nvs_config.h"
#include "config.h"

static const char *TAG = LOG_TAG_SCANNER;

static TaskHandle_t scanner_task_handle = NULL;
static bool task_running = false;
static scanner_state_t current_state = SCANNER_IDLE;

static char ez_ssid[33] = {0};
static char ez_pass[65] = {0};

static int64_t last_proxy_time_us = 0;

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

static esp_err_t send_proxy_chunk(int req_id, size_t seq, bool is_last,
                                   const uint8_t *data, size_t data_len)
{
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, data, data_len);

    char *b64_buf = malloc(b64_len + 1);
    if (!b64_buf) return ESP_ERR_NO_MEM;

    size_t actual = 0;
    mbedtls_base64_encode((unsigned char *)b64_buf, b64_len, &actual, data, data_len);
    b64_buf[actual] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "proxy_chunk");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON_AddNumberToObject(root, "seq", seq);
    cJSON_AddBoolToObject(root, "last", is_last);
    cJSON_AddStringToObject(root, "d", b64_buf);

    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (json) { err = uart_send_json(json); free(json); }

    free(b64_buf);
    cJSON_Delete(root);
    return err;
}

/* ── Chunk callback — sends meta + chunks over UART ────────────── */

static esp_err_t proxy_chunk_callback(const uint8_t *data, size_t len,
                                       size_t seq, bool is_last, void *ctx)
{
    proxy_ctx_t *pctx = (proxy_ctx_t *)ctx;

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

    if (ssid_j && cJSON_IsString(ssid_j)) {
        const char *new_pass = (pass_j && cJSON_IsString(pass_j)) ? pass_j->valuestring : "";
        nvs_config_set_ezshare(ssid_j->valuestring, new_pass);
        ESP_LOGI(TAG, "set_config: SSID=%s", ssid_j->valuestring);

        cJSON *ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "type", "config_ack");
        cJSON_AddStringToObject(ack, "status", "ok");
        char *json = cJSON_PrintUnformatted(ack);
        if (json) { uart_send_json(json); free(json); }
        cJSON_Delete(ack);

        uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(2000));
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

/* ── Main loop ─────────────────────────────────────────────────── */

static void scanner_task_loop(void *pvParameters)
{
    char uart_buf[JSON_BUFFER_SIZE];

    while (task_running) {
        switch (current_state) {
            case SCANNER_IDLE: {
                check_idle_disconnect();

                int len = uart_receive_json(uart_buf, sizeof(uart_buf), 1000);
                if (len <= 0) {
                    vTaskDelay(pdMS_TO_TICKS(SCANNER_POLL_INTERVAL_MS));
                    break;
                }

                cJSON *root = cJSON_Parse(uart_buf);
                if (!root) break;

                cJSON *type = cJSON_GetObjectItem(root, "type");
                if (!type || !cJSON_IsString(type)) { cJSON_Delete(root); break; }

                if (strcmp(type->valuestring, "set_config") == 0) {
                    handle_set_config(root);
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

                if (err != ESP_OK || pctx.error) {
                    ESP_LOGE(TAG, "proxy failed: %s", esp_err_to_name(err));
                    if (!pctx.meta_sent)
                        send_error_json(proxy_req_id, "ezShare request failed", "HTTP_FAILED");
                }

                last_proxy_time_us = esp_timer_get_time();
                current_state = SCANNER_IDLE;
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
