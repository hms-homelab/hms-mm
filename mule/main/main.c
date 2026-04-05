/**
 * @file main.c
 * @brief Mule — receives files from miner via UART, serves via HTTP.
 *
 * Boot flow:
 *   1. NVS init
 *   2. UART init (needed for captive portal to send creds to miner)
 *   3. Check NVS for home WiFi creds
 *      - If NVS has creds -> connect
 *      - Else if Kconfig defaults are set -> connect
 *      - Else -> captive portal (collects home WiFi + ezShare, sends ezShare to miner, reboots)
 *   4. If connect fails with NVS creds -> clear NVS, reboot into portal
 *   5. Start HTTP file server + mule task
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
#include "file_cache.h"
#include "file_server.h"
#include "mule_task.h"
#include "config.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "=== hms-mm mule starting ===");

    // NVS
    nvs_config_init();

    // UART (must be up before captive portal — portal sends ezShare creds to miner)
    uart_handler_init();
    ESP_LOGI(TAG, "UART: TX=GPIO%d, RX=GPIO%d", UART_TX_PIN, UART_RX_PIN);

    // WiFi: NVS -> Kconfig -> captive portal
    char ssid[33] = {0}, pass[65] = {0};
    bool wifi_ok = false;

    if (nvs_config_has_wifi()) {
        nvs_config_get_wifi_ssid(ssid, sizeof(ssid));
        nvs_config_get_wifi_pass(pass, sizeof(pass));
        ESP_LOGI(TAG, "Using NVS WiFi (SSID: %s)", ssid);

        wifi_manager_init();
        wifi_ok = (wifi_manager_connect(ssid, pass, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK);

        if (!wifi_ok) {
            ESP_LOGW(TAG, "NVS WiFi failed, starting captive portal");
            // Don't clear NVS here — let user re-enter via portal
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
            ESP_LOGW(TAG, "Kconfig WiFi failed, starting captive portal");
            captive_portal_start();
            return;
        }
    } else {
        ESP_LOGI(TAG, "No WiFi credentials, starting captive portal");
        captive_portal_start();
        return;
    }

    // WiFi connected — start HTTP server
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &http_cfg) == ESP_OK) {
        file_server_register(server);
        ESP_LOGI(TAG, "HTTP file server started on port 80");
    }

    // Start mule task (periodic file collection from miner)
    mule_task_init();
    mule_task_start();

    ESP_LOGI(TAG, "=== mule running — serving files + collecting from miner ===");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        mule_state_t state = mule_task_get_state();
        const char *s = (state == MULE_IDLE) ? "IDLE" :
                        (state == MULE_REQUESTING) ? "REQUESTING" :
                        (state == MULE_RECEIVING) ? "RECEIVING" :
                        (state == MULE_DECODING) ? "DECODING" : "ERROR";
        ESP_LOGI(TAG, "State: %s | WiFi: %s | Cache: %zu files",
                 s, wifi_manager_is_connected() ? "OK" : "DOWN",
                 file_cache_count());
    }
}
