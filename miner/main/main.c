/**
 * @file main.c
 * @brief Miner — connects to ezShare, downloads files, sends to mule via UART.
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
    ESP_LOGI(TAG, "=== hms-mm miner starting ===");

    // NVS (stores ezShare creds received from mule)
    nvs_config_init();

    // UART (communication with mule)
    uart_handler_init();
    ESP_LOGI(TAG, "UART: TX=GPIO%d, RX=GPIO%d", UART_TX_PIN, UART_RX_PIN);

    // WiFi manager (used to connect to ezShare on demand)
    wifi_manager_init();

    // ezShare HTTP client
    ezshare_client_init();

    // Scanner task (state machine: IDLE -> CONNECTING -> LISTING -> DOWNLOADING -> SENDING)
    scanner_task_init();
    scanner_task_start();

    ESP_LOGI(TAG, "=== miner running — waiting for UART commands ===");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        scanner_state_t state = scanner_task_get_state();
        const char *s = (state == SCANNER_IDLE) ? "IDLE" :
                        (state == SCANNER_CONNECTING) ? "CONNECTING" :
                        (state == SCANNER_LISTING) ? "LISTING" :
                        (state == SCANNER_DOWNLOADING) ? "DOWNLOADING" :
                        (state == SCANNER_SENDING) ? "SENDING" :
                        (state == SCANNER_DISCONNECTING) ? "DISCONNECTING" : "ERROR";
        ESP_LOGI(TAG, "State: %s", s);
    }
}
