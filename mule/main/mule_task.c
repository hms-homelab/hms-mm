/**
 * @file mule_task.c
 * @brief Mule task — sends ezShare config to miner at boot, then idles.
 *
 * All proxy work happens in file_server.c HTTP handlers.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mule_task.h"
#include "uart_handler.h"
#include "nvs_config.h"
#include "config.h"

static const char *TAG = LOG_TAG_MULE;

static TaskHandle_t s_task = NULL;
static bool s_running = false;

static void send_ezshare_config(void)
{
    char ez_ssid[33] = {0}, ez_pass[65] = {0};
    if (nvs_config_has_ezshare()) {
        nvs_config_get_ezshare_ssid(ez_ssid, sizeof(ez_ssid));
        nvs_config_get_ezshare_pass(ez_pass, sizeof(ez_pass));
    }

    if (ez_ssid[0] == '\0') return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "set_config");
    cJSON_AddStringToObject(root, "ez_ssid", ez_ssid);
    cJSON_AddStringToObject(root, "ez_pass", ez_pass);
    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        ESP_LOGI(TAG, "Sending ezShare config to miner (SSID: %s)", ez_ssid);
        uart_send_json(json);
        free(json);
    }
    cJSON_Delete(root);
}

static void mule_task_loop(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(MULE_BOOT_DELAY_SEC * 1000));
    send_ezshare_config();

    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    vTaskDelete(NULL);
}

esp_err_t mule_task_init(void)
{
    s_running = false;
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
mule_state_t mule_task_get_state(void) { return MULE_IDLE; }
