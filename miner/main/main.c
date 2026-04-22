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

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "=== hms-mm miner starting (proxy mode) ===");

    nvs_config_init();
    uart_handler_init();
    wifi_manager_init();
    ezshare_client_init();

    scanner_task_init();
    scanner_task_start();

    ESP_LOGI(TAG, "=== miner running — waiting for UART proxy requests ===");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        const char *s = (scanner_task_get_state() == SCANNER_IDLE) ? "IDLE" :
                        (scanner_task_get_state() == SCANNER_PROXY) ? "PROXY" : "ERROR";
        ESP_LOGI(TAG, "State: %s | WiFi: %s", s,
                 wifi_manager_is_connected() ? "ezShare" : "disconnected");
    }
}
