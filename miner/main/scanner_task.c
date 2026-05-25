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
#include "o2ring_ble.h"

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

#define O2RING_DL_BUF_SIZE (48 * 1024)

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

                if (err != ESP_OK || pctx.error) {
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

                    /* Heap-allocate download buffer (WiFi is off, ~60KB free) */
                    uint8_t *dl_buf = malloc(O2RING_DL_BUF_SIZE);
                    if (!dl_buf) {
                        send_error_json(o2ring_req_id, "Out of memory", "OOM");
                        current_state = SCANNER_IDLE;
                        break;
                    }

                    size_t out_len = 0;
                    esp_err_t ret = o2ring_ble_download_file(o2ring_filename, dl_buf,
                                                             O2RING_DL_BUF_SIZE, &out_len);
                    if (ret != ESP_OK || out_len == 0) {
                        send_error_json(o2ring_req_id, "File download failed", "BLE_READ_FAIL");
                        free(dl_buf);
                        current_state = SCANNER_IDLE;
                        break;
                    }

                    /* Send proxy_meta with actual size, then chunked data */
                    send_proxy_meta(o2ring_req_id, 200, (uint32_t)out_len, 0);

                    size_t offset = 0;
                    size_t seq = 0;
                    while (offset < out_len) {
                        size_t chunk_len = out_len - offset;
                        if (chunk_len > FILE_CHUNK_SIZE) chunk_len = FILE_CHUNK_SIZE;
                        bool is_last = (offset + chunk_len >= out_len);
                        send_proxy_chunk(o2ring_req_id, seq, is_last,
                                         dl_buf + offset, chunk_len);
                        offset += chunk_len;
                        seq++;
                    }
                    free(dl_buf);
                } else {
                    send_error_json(o2ring_req_id, "Unknown o2ring command", "INVALID_CMD");
                }
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
