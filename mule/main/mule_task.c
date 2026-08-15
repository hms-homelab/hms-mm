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
#include "esp_timer.h"
#include "mule_task.h"
#include "uart_handler.h"
#include "miner_link.h"
#include "nvs_config.h"
#include "config.h"

static const char *TAG = LOG_TAG_MULE;

static TaskHandle_t s_task = NULL;
static bool s_running = false;

/* Returns true if the miner said it is restarting to apply new credentials. */
static bool send_ezshare_config(void)
{
    char ez_ssid[33] = {0}, ez_pass[65] = {0};
    if (nvs_config_has_ezshare()) {
        nvs_config_get_ezshare_ssid(ez_ssid, sizeof(ez_ssid));
        nvs_config_get_ezshare_pass(ez_pass, sizeof(ez_pass));
    }

    if (ez_ssid[0] == '\0') return false;

    bool restarting = false;

    /* Hold the link across send-and-ack: this runs concurrently with the HTTP
     * handlers, and without the lock a client request landing here would
     * consume the config_ack. */
    if (!uart_link_lock(5000)) {
        ESP_LOGW(TAG, "link busy — sending ezShare config unacknowledged");
    }

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

    /* Wait for the ack. It was previously sent by the miner and read by nobody,
     * so a miner that never received the config looked identical to one that
     * did. It also tells us whether the miner is about to restart. */
    static char buf[512];
    int64_t deadline = esp_timer_get_time() + 3000 * 1000;
    bool acked = false;
    while (!acked && esp_timer_get_time() < deadline) {
        if (uart_receive_json(buf, sizeof(buf), 300) <= 0) continue;
        cJSON *msg = cJSON_Parse(buf);
        if (!msg) continue;
        cJSON *type = cJSON_GetObjectItem(msg, "type");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "config_ack") == 0) {
            cJSON *ch = cJSON_GetObjectItem(msg, "changed");
            restarting = cJSON_IsTrue(ch);
            acked = true;
        }
        cJSON_Delete(msg);
    }

    uart_link_unlock();

    if (!acked)
        ESP_LOGW(TAG, "miner did not acknowledge ezShare config — is the link wired?");
    else
        ESP_LOGI(TAG, "miner acknowledged config (%s)",
                 restarting ? "restarting to apply" : "already current");

    return restarting;
}

static void mule_task_loop(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(MULE_BOOT_DELAY_SEC * 1000));
    bool miner_restarting = send_ezshare_config();

    /* Let the miner finish rebooting before asking it anything, or the version
     * query just times out and caches "unknown" until something else asks. */
    if (miner_restarting) vTaskDelay(pdMS_TO_TICKS(4000));

    if (!miner_link_query_version(NULL, 0))
        ESP_LOGW(TAG, "could not read miner firmware version at boot");

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
