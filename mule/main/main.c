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
#include "esp_system.h"
#include "esp_http_server.h"
#include "uart_handler.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "captive_portal.h"
#include "file_server.h"
#include "control_server.h"
#include "log_ring.h"
#include "ota_service.h"
#include "crash_guard.h"
#include "mule_task.h"
#include "config.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    /* Before the first log line, so the ring captures the whole boot. */
    log_ring_init();

    ESP_LOGI(TAG, "=== %s mule v%s (proxy mode) ===", FW_PROJECT, FW_VERSION);

    nvs_config_init();
    /* After NVS (it may need to clear credentials) and before anything that
     * could itself crash. */
    crash_guard_init();
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
            /* Do NOT jump straight to the setup portal. The overwhelmingly
             * common reason a known-good device fails to join at boot is that
             * the router is not back yet, and dropping into setup mode over
             * that would strand a working unit and demand re-provisioning for
             * a problem that fixes itself. Retry across reboots, and only
             * conclude the credentials are wrong once it has failed
             * persistently. */
            uint32_t fails = nvs_config_wifi_fail_bump();
            if (fails < WIFI_FAIL_THRESHOLD) {
                ESP_LOGW(TAG, "WiFi join failed (%lu/%d) — keeping credentials, retrying",
                         (unsigned long)fails, WIFI_FAIL_THRESHOLD);
                vTaskDelay(pdMS_TO_TICKS(5000));
                esp_restart();
            }
            ESP_LOGE(TAG, "WiFi join failed %lu times — clearing credentials for setup",
                     (unsigned long)fails);
            nvs_config_clear_wifi();
            captive_portal_start();
            return;
        }
        nvs_config_wifi_fail_clear();
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

    /* control_server owns the httpd and mounts the proxy routes onto it, so
     * the device page, the status endpoint and the data routes all live on one
     * server sharing one link lock. */
    if (control_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server, rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    /* WiFi is up and the server is serving, which is as much as this board can
     * prove about itself. Confirm now so the bootloader stops holding the old
     * image in reserve. */
    ota_service_confirm_boot();
    /* Serving HTTP proves more than uptime does. */
    crash_guard_mark_healthy();

    mule_task_init();
    mule_task_start();

    ESP_LOGI(TAG, "=== mule running — proxy mode ===");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "WiFi: %s", wifi_manager_is_connected() ? "OK" : "DOWN");
    }
}
