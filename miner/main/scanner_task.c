/**
 * @file scanner_task.c
 * @brief Miner state machine — downloads files from ezShare, sends via UART.
 *        Handles set_config from mule to store ezShare creds in NVS.
 */

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
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

static char requested_timestamp[32] = {0};
static time_t requested_timestamp_unix = 0;
static ezshare_file_list_t file_list = {0};

// Active ezShare creds (loaded from NVS or defaults at boot)
static char ez_ssid[33] = {0};
static char ez_pass[65] = {0};

void scanner_task_load_ezshare_creds(void)
{
    if (nvs_config_has_ezshare()) {
        nvs_config_get_ezshare_ssid(ez_ssid, sizeof(ez_ssid));
        nvs_config_get_ezshare_pass(ez_pass, sizeof(ez_pass));
        ESP_LOGI(TAG, "Using NVS ezShare creds (SSID: %s)", ez_ssid);
    } else {
        strncpy(ez_ssid, EZSHARE_WIFI_SSID_DEFAULT, sizeof(ez_ssid));
        strncpy(ez_pass, EZSHARE_WIFI_PASSWORD_DEFAULT, sizeof(ez_pass));
        ESP_LOGI(TAG, "Using default ezShare creds (SSID: %s)", ez_ssid);
    }
}

static time_t parse_iso8601_timestamp(const char *timestamp_str)
{
    struct tm tm = {0};
    if (sscanf(timestamp_str, "%d-%d-%dT%d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return 0;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return mktime(&tm);
}

static void send_error_message(const char *error_msg, const char *error_code)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "message", error_msg);
    cJSON_AddStringToObject(root, "code", error_code);
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) { uart_send_json(json_str); free(json_str); }
    cJSON_Delete(root);
}

static esp_err_t send_file_via_uart(const ezshare_file_t *file)
{
    size_t base64_len = 0;
    mbedtls_base64_encode(NULL, 0, &base64_len, file->content, file->size);

    char *base64_content = malloc(base64_len + 1);
    if (!base64_content) return ESP_ERR_NO_MEM;

    size_t actual_len = 0;
    if (mbedtls_base64_encode((unsigned char *)base64_content, base64_len,
                               &actual_len, file->content, file->size) != 0) {
        free(base64_content);
        return ESP_FAIL;
    }
    base64_content[actual_len] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "file_data");
    cJSON_AddStringToObject(root, "path", file->path);
    cJSON_AddNumberToObject(root, "size", file->size);
    cJSON_AddStringToObject(root, "content_base64", base64_content);

    char *json_str = cJSON_PrintUnformatted(root);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (json_str) { err = uart_send_json(json_str); free(json_str); }

    free(base64_content);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sent file: %s (%d bytes)", file->path, file->size);
    }
    return err;
}

static void send_file_array_complete(void)
{
    size_t total_bytes = 0;
    for (size_t i = 0; i < file_list.count; i++) total_bytes += file_list.files[i].size;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "file_array_complete");
    cJSON_AddNumberToObject(root, "count", file_list.count);
    cJSON_AddNumberToObject(root, "total_bytes", total_bytes);
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) { uart_send_json(json_str); free(json_str); }
    cJSON_Delete(root);
}

/**
 * Handle set_config from mule: store ezShare creds in NVS and reboot.
 */
static void handle_set_config(cJSON *root)
{
    cJSON *ez_ssid_j = cJSON_GetObjectItem(root, "ez_ssid");
    cJSON *ez_pass_j = cJSON_GetObjectItem(root, "ez_pass");

    if (ez_ssid_j && cJSON_IsString(ez_ssid_j)) {
        const char *new_ssid = ez_ssid_j->valuestring;
        const char *new_pass = (ez_pass_j && cJSON_IsString(ez_pass_j))
                               ? ez_pass_j->valuestring : "";

        nvs_config_set_ezshare(new_ssid, new_pass);
        ESP_LOGI(TAG, "Received set_config from mule — ezShare SSID: %s", new_ssid);

        // ACK back to mule
        cJSON *ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "type", "config_ack");
        cJSON_AddStringToObject(ack, "status", "ok");
        char *json_str = cJSON_PrintUnformatted(ack);
        if (json_str) { uart_send_json(json_str); free(json_str); }
        cJSON_Delete(ack);

        // Delay then reboot
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Rebooting with new ezShare credentials...");
        esp_restart();
    }
}

static void scanner_task_loop(void *pvParameters)
{
    char uart_buffer[JSON_BUFFER_SIZE];
    char date_folders[MAX_DATE_FOLDERS][16];
    size_t folder_count = 0;

    while (task_running) {
        switch (current_state) {
            case SCANNER_IDLE: {
                int len = uart_receive_json(uart_buffer, sizeof(uart_buffer), 1000);
                if (len > 0) {
                    cJSON *root = cJSON_Parse(uart_buffer);
                    if (root) {
                        cJSON *type = cJSON_GetObjectItem(root, "type");
                        if (type && cJSON_IsString(type)) {
                            if (strcmp(type->valuestring, "set_config") == 0) {
                                handle_set_config(root);
                            } else if (strcmp(type->valuestring, "timestamp_req") == 0) {
                                cJSON *ts = cJSON_GetObjectItem(root, "timestamp");
                                if (ts && cJSON_IsString(ts)) {
                                    strncpy(requested_timestamp, ts->valuestring,
                                            sizeof(requested_timestamp) - 1);
                                    requested_timestamp_unix = parse_iso8601_timestamp(ts->valuestring);
                                    current_state = SCANNER_CONNECTING;
                                }
                            } else if (strcmp(type->valuestring, "get_latest_req") == 0) {
                                cJSON *max_age = cJSON_GetObjectItem(root, "max_age_hours");
                                int hours = max_age ? max_age->valueint : 24;
                                requested_timestamp_unix = time(NULL) - (hours * 3600);
                                current_state = SCANNER_CONNECTING;
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(SCANNER_POLL_INTERVAL_MS));
                break;
            }

            case SCANNER_CONNECTING:
                ESP_LOGI(TAG, "Connecting to ezShare WiFi (%s)...", ez_ssid);
                if (wifi_manager_connect(ez_ssid, ez_pass, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK) {
                    current_state = SCANNER_LISTING;
                } else {
                    send_error_message("Failed to connect to ezShare WiFi", "WIFI_CONNECT_FAILED");
                    current_state = SCANNER_ERROR;
                }
                break;

            case SCANNER_LISTING:
                if (ezshare_list_date_folders(date_folders, MAX_DATE_FOLDERS, &folder_count) != ESP_OK) {
                    send_error_message("Failed to list date folders", "HTTP_REQUEST_FAILED");
                    current_state = SCANNER_ERROR;
                    break;
                }
                ezshare_file_list_init(&file_list, 10);
                for (size_t i = 0; i < folder_count; i++) {
                    ezshare_list_files(date_folders[i], &file_list, requested_timestamp_unix);
                }
                current_state = (file_list.count > 0) ? SCANNER_DOWNLOADING : SCANNER_DISCONNECTING;
                break;

            case SCANNER_DOWNLOADING:
                for (size_t i = 0; i < file_list.count; i++) {
                    uint8_t *content = NULL;
                    size_t size = 0;
                    if (ezshare_download_file(file_list.files[i].path, &content, &size) == ESP_OK) {
                        file_list.files[i].content = content;
                        file_list.files[i].size = size;
                    }
                }
                current_state = SCANNER_SENDING;
                break;

            case SCANNER_SENDING:
                for (size_t i = 0; i < file_list.count; i++) {
                    if (file_list.files[i].content) {
                        send_file_via_uart(&file_list.files[i]);
                    }
                }
                send_file_array_complete();
                current_state = SCANNER_DISCONNECTING;
                break;

            case SCANNER_DISCONNECTING:
                wifi_manager_disconnect();
                ezshare_file_list_free(&file_list);
                current_state = SCANNER_IDLE;
                break;

            case SCANNER_ERROR:
                if (wifi_manager_is_connected()) wifi_manager_disconnect();
                if (file_list.files) ezshare_file_list_free(&file_list);
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

void scanner_task_stop(void)
{
    task_running = false;
}

scanner_state_t scanner_task_get_state(void)
{
    return current_state;
}
