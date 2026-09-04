/**
 * @file main.c
 * @brief Mule — proxies HTTP requests to miner via UART.
 *
 * Boot flow:
 *   1. NVS init
 *   2. UART init
 *   3. USB provisioning listener (browser flasher)
 *   4. WiFi: NVS -> Kconfig -> captive portal
 *   5. Start HTTP proxy server + mule task (sends ezShare config to miner)
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
#include "miner_link.h"
#include "mule_task.h"
#include "serial_config.h"
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

    /* Before the link probe and everything after it: each of the branches
     * below can end in a restart or the setup portal, and the browser flasher
     * has to be able to provision the mule in all of them. */
    serial_config_start();

    /* Probe the link before touching WiFi. The miner version was previously
     * only read once the network was up, so a unit that could not join told
     * you nothing about whether the two boards were even wired to each other,
     * and a link fault looked identical to a WiFi fault. This separates them:
     * by the next line you know whether the miner is there. */
    char mver[32] = {0};
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (miner_link_query_version(mver, sizeof(mver))) {
            ESP_LOGI(TAG, "Link OK: miner firmware %s", mver);
            break;
        }
        if (attempt < 3) {
            ESP_LOGW(TAG, "No answer from the miner (%d/3), retrying", attempt);
            vTaskDelay(pdMS_TO_TICKS(1500));   /* it may still be booting */
        } else {
            ESP_LOGE(TAG, "Link DOWN: the miner did not answer. Check the "
                          "TX/RX crossover (mule GPIO%d/%d) and a common ground.",
                     UART_TX_PIN, UART_RX_PIN);
        }
    }

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

            /* How long to persist depends on WHY it failed. The device already
             * knows: an absent network (a router still booting) deserves
             * patience, while credentials the AP actively rejects will never
             * start working on their own, and making someone wait five minutes
             * to retype a password is its own kind of broken. */
            bool auth = wifi_manager_last_failure_was_auth();
            uint32_t limit = auth ? WIFI_AUTH_FAIL_THRESHOLD : WIFI_FAIL_THRESHOLD;

            if (fails < limit) {
                ESP_LOGW(TAG, "WiFi join failed (%lu/%lu, reason=%d%s) — keeping credentials, retrying",
                         (unsigned long)fails, (unsigned long)limit,
                         wifi_manager_last_disc_reason(),
                         auth ? ", looks like bad credentials" : "");
                vTaskDelay(pdMS_TO_TICKS(auth ? 1000 : 5000));
                esp_restart();
            }
            ESP_LOGE(TAG, "WiFi join failed %lu times (reason=%d) — clearing credentials for setup",
                     (unsigned long)fails, wifi_manager_last_disc_reason());
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
