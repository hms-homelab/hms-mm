/**
 * @file mule_task.c
 * @brief Mule state machine — periodically requests files from miner,
 *        receives via UART, decodes, caches for HTTP serving.
 */

#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mule_task.h"
#include "uart_handler.h"
#include "file_cache.h"
#include "config.h"

static const char *TAG = LOG_TAG_MULE;

static TaskHandle_t s_task = NULL;
static bool s_running = false;
static mule_state_t s_state = MULE_IDLE;

// Pending files (base64-encoded, waiting for decode)
typedef struct {
    char path[256];
    size_t raw_size;
    char *content_base64;
} pending_file_t;

static pending_file_t *s_pending = NULL;
static size_t s_pending_count = 0;
static size_t s_pending_cap = 0;
static bool s_array_complete = false;

static void free_pending(void)
{
    if (s_pending) {
        for (size_t i = 0; i < s_pending_count; i++)
            free(s_pending[i].content_base64);
        free(s_pending);
        s_pending = NULL;
    }
    s_pending_count = 0;
    s_pending_cap = 0;
    s_array_complete = false;
}

static void add_pending(const char *path, size_t size, const char *b64)
{
    if (s_pending_count >= s_pending_cap) {
        size_t new_cap = s_pending_cap ? s_pending_cap * 2 : 10;
        s_pending = realloc(s_pending, new_cap * sizeof(pending_file_t));
        s_pending_cap = new_cap;
    }
    strncpy(s_pending[s_pending_count].path, path, sizeof(s_pending[0].path) - 1);
    s_pending[s_pending_count].raw_size = size;
    s_pending[s_pending_count].content_base64 = strdup(b64);
    s_pending_count++;
}

static void mule_task_loop(void *arg)
{
    char uart_buf[JSON_BUFFER_SIZE];
    time_t last_collection = 0;
    time_t boot_time = time(NULL);

    while (s_running) {
        switch (s_state) {
            case MULE_IDLE: {
                time_t now = time(NULL);

                // Boot delay
                if ((now - boot_time) < MULE_BOOT_DELAY_SEC) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    break;
                }

                // Check collection interval
                if ((now - last_collection) >= MULE_COLLECTION_INTERVAL_SEC) {
                    ESP_LOGI(TAG, "Requesting files from miner (last %d hours)", MULE_MAX_AGE_HOURS);

                    cJSON *req = cJSON_CreateObject();
                    cJSON_AddStringToObject(req, "type", "get_latest_req");
                    cJSON_AddNumberToObject(req, "max_age_hours", MULE_MAX_AGE_HOURS);
                    char *json = cJSON_PrintUnformatted(req);
                    if (json) {
                        uart_send_json(json);
                        free(json);
                        free_pending();
                        s_state = MULE_RECEIVING;
                        last_collection = now;
                    }
                    cJSON_Delete(req);
                }

                vTaskDelay(pdMS_TO_TICKS(MULE_POLL_INTERVAL_MS));
                break;
            }

            case MULE_RECEIVING: {
                int len = uart_receive_json(uart_buf, sizeof(uart_buf), 5000);
                if (len > 0) {
                    cJSON *msg = cJSON_Parse(uart_buf);
                    if (msg) {
                        cJSON *type = cJSON_GetObjectItem(msg, "type");
                        if (type && cJSON_IsString(type)) {
                            if (strcmp(type->valuestring, "file_data") == 0) {
                                cJSON *path = cJSON_GetObjectItem(msg, "path");
                                cJSON *size = cJSON_GetObjectItem(msg, "size");
                                cJSON *b64  = cJSON_GetObjectItem(msg, "content_base64");
                                if (path && size && b64) {
                                    add_pending(path->valuestring, size->valueint, b64->valuestring);
                                    ESP_LOGI(TAG, "Received file %zu: %s",
                                             s_pending_count, path->valuestring);
                                }
                            } else if (strcmp(type->valuestring, "file_array_complete") == 0) {
                                s_array_complete = true;
                                s_state = MULE_DECODING;
                            } else if (strcmp(type->valuestring, "error") == 0) {
                                cJSON *em = cJSON_GetObjectItem(msg, "message");
                                ESP_LOGE(TAG, "Miner error: %s",
                                         em ? em->valuestring : "unknown");
                                s_state = MULE_ERROR;
                            }
                        }
                        cJSON_Delete(msg);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(MULE_POLL_INTERVAL_MS));
                break;
            }

            case MULE_DECODING:
                ESP_LOGI(TAG, "Decoding %zu files into cache", s_pending_count);

                for (size_t i = 0; i < s_pending_count; i++) {
                    if (!s_pending[i].content_base64) continue;

                    size_t decoded_len = 0;
                    mbedtls_base64_decode(NULL, 0, &decoded_len,
                                          (const unsigned char *)s_pending[i].content_base64,
                                          strlen(s_pending[i].content_base64));

                    uint8_t *decoded = malloc(decoded_len);
                    if (!decoded) continue;

                    size_t actual = 0;
                    if (mbedtls_base64_decode(decoded, decoded_len, &actual,
                                              (const unsigned char *)s_pending[i].content_base64,
                                              strlen(s_pending[i].content_base64)) == 0) {
                        // file_cache_put takes ownership of decoded pointer
                        file_cache_put(s_pending[i].path, decoded, actual);
                    } else {
                        free(decoded);
                        ESP_LOGE(TAG, "Base64 decode failed: %s", s_pending[i].path);
                    }
                }

                ESP_LOGI(TAG, "Cache: %zu files, %zu bytes total",
                         file_cache_count(), file_cache_total_bytes());

                free_pending();
                s_state = MULE_IDLE;
                break;

            case MULE_REQUESTING:
                // Transient state — immediately transitions to RECEIVING
                s_state = MULE_RECEIVING;
                break;

            case MULE_ERROR:
                free_pending();
                vTaskDelay(pdMS_TO_TICKS(10000));
                s_state = MULE_IDLE;
                break;
        }
    }
    vTaskDelete(NULL);
}

esp_err_t mule_task_init(void)
{
    s_state = MULE_IDLE;
    s_running = false;
    file_cache_init();
    return ESP_OK;
}

esp_err_t mule_task_start(void)
{
    if (s_running) return ESP_OK;
    s_running = true;
    if (xTaskCreate(mule_task_loop, "mule_task", MULE_TASK_STACK_SIZE,
                    NULL, MULE_TASK_PRIORITY, &s_task) != pdPASS) {
        s_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void mule_task_stop(void) { s_running = false; }
mule_state_t mule_task_get_state(void) { return s_state; }
