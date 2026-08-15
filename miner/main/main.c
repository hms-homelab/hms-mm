/**
 * @file main.c
 * @brief Miner — proxies HTTP requests from mule to ezShare via UART.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "uart_handler.h"
#include "wifi_manager.h"
#include "ezshare_client.h"
#include "scanner_task.h"
#include "nvs_config.h"
#include "config.h"
#include "ota_handler.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "=== %s miner v%s (proxy mode) ===", FW_PROJECT, FW_VERSION);

    nvs_config_init();
    uart_handler_init();
    wifi_manager_init();
    ezshare_client_init();

    /* If this boot is running a freshly written image, it now has a deadline
     * to prove it can still hear the mule. Arm before the scanner starts, so
     * the first decoded frame can confirm it. */
    miner_ota_arm_rollback_watchdog();

    scanner_task_init();
    scanner_task_start();

    ESP_LOGI(TAG, "=== miner running — waiting for UART proxy requests ===");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        /* Name every state. The old form listed two and called everything
         * else "ERROR", so a healthy miner serving an O2Ring request reported
         * itself as failed. */
        const char *s;
        switch (scanner_task_get_state()) {
            case SCANNER_IDLE:   s = "IDLE";   break;
            case SCANNER_PROXY:  s = "PROXY";  break;
            case SCANNER_O2RING: s = "O2RING"; break;
            case SCANNER_OTA:    s = "OTA";    break;
            case SCANNER_ERROR:  s = "ERROR";  break;
            default:             s = "UNKNOWN"; break;
        }
        ESP_LOGI(TAG, "State: %s | WiFi: %s", s,
                 wifi_manager_is_connected() ? "ezShare" : "disconnected");
    }
}
