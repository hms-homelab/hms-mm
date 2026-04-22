/**
 * @file main.c
 * @brief Mule — proxies HTTP requests to miner via UART.
 *
 * Boot flow:
 *   1. NVS init
 *   2. UART init
 *   3. WiFi: NVS -> Kconfig -> captive portal
 *   4. Start HTTP proxy server + mule task (sends ezShare config to miner)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "uart_handler.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "captive_portal.h"
#include "file_server.h"
#include "mule_task.h"
#include "config.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "=== %s mule v%s (proxy mode) ===", FW_PROJECT, FW_VERSION);

    nvs_config_init();
    uart_handler_init();

    char ssid[33] = {0}, pass[65] = {0};
    bool wifi_ok = false;

    if (nvs_config_has_wifi()) {
        nvs_config_get_wifi_ssid(ssid, sizeof(ssid));
        nvs_config_get_wifi_pass(pass, sizeof(pass));
        ESP_LOGI(TAG, "Using NVS WiFi (SSID: %s)", ssid);
        wifi_manager_init();
        wifi_ok = (wifi_manager_connect(ssid, pass, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK);
        if (!wifi_ok) {
            captive_portal_start();
            return;
        }
    } else if (strlen(HOME_WIFI_SSID_DEFAULT) > 0 &&
               strcmp(HOME_WIFI_SSID_DEFAULT, "your_wifi_ssid") != 0) {
        ESP_LOGI(TAG, "Using Kconfig WiFi (SSID: %s)", HOME_WIFI_SSID_DEFAULT);
        wifi_manager_init();
        wifi_ok = (wifi_manager_connect(HOME_WIFI_SSID_DEFAULT, HOME_WIFI_PASSWORD_DEFAULT,
                                         WIFI_CONNECT_TIMEOUT_MS) == ESP_OK);
        if (!wifi_ok) {
            captive_portal_start();
            return;
        }
    } else {
        captive_portal_start();
        return;
    }

    file_server_init();

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.stack_size = 8192;
    http_cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &http_cfg) == ESP_OK) {
        file_server_register(server);
        ESP_LOGI(TAG, "HTTP proxy server started on port 80");
    }

    mule_task_init();
    mule_task_start();

    ESP_LOGI(TAG, "=== mule running — proxy mode ===");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "WiFi: %s", wifi_manager_is_connected() ? "OK" : "DOWN");
    }
}
