/**
 * @file wifi_manager.c
 * @brief Scanner C3 WiFi Manager - ez Share WiFi Connection
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "config.h"

static const char *TAG = LOG_TAG_WIFI;

// Event group for WiFi events
static EventGroupHandle_t wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;

// Connection state
static wifi_status_t wifi_status = WIFI_STATUS_DISCONNECTED;
static int retry_count = 0;
static bool wifi_initialized = false;

/**
 * @brief WiFi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started, connecting...");
        esp_wifi_connect();
        wifi_status = WIFI_STATUS_CONNECTING;

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < WIFI_MAXIMUM_RETRY) {
            ESP_LOGI(TAG, "WiFi disconnected, retrying... (%d/%d)",
                     retry_count + 1, WIFI_MAXIMUM_RETRY);
            esp_wifi_connect();
            retry_count++;
            wifi_status = WIFI_STATUS_CONNECTING;
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAXIMUM_RETRY);
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            wifi_status = WIFI_STATUS_ERROR;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_status = WIFI_STATUS_CONNECTED;
    }
}

/**
 * @brief Initialize WiFi manager
 */
esp_err_t wifi_manager_init(void) {
    if (wifi_initialized) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return ESP_OK;
    }

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create WiFi station interface
    esp_netif_create_default_wifi_sta();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create event group
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    // Set WiFi mode to station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_initialized = true;
    wifi_status = WIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi manager initialized");

    return ESP_OK;
}

/**
 * @brief Connect to WiFi network
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (!wifi_initialized) {
        ESP_LOGE(TAG, "WiFi manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL) {
        ESP_LOGE(TAG, "NULL SSID");
        return ESP_ERR_INVALID_ARG;
    }

    // Configure WiFi connection
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    // Set WiFi configuration
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }

    // Clear event bits
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Start WiFi
    retry_count = 0;
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed");
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
        return ESP_ERR_TIMEOUT;
    }
}

/**
 * @brief Disconnect from WiFi network
 */
esp_err_t wifi_manager_disconnect(void) {
    if (!wifi_initialized) {
        ESP_LOGW(TAG, "WiFi manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (wifi_status == WIFI_STATUS_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi already disconnected");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Disconnecting WiFi...");

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disconnect WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_status = WIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi disconnected");

    return ESP_OK;
}

/**
 * @brief Get current WiFi connection status
 */
wifi_status_t wifi_manager_get_status(void) {
    return wifi_status;
}

/**
 * @brief Check if WiFi is connected
 */
bool wifi_manager_is_connected(void) {
    return (wifi_status == WIFI_STATUS_CONNECTED);
}

/**
 * @brief Wait for WiFi connection (blocking)
 */
esp_err_t wifi_manager_wait_connection(uint32_t timeout_ms) {
    if (!wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    } else {
        return ESP_ERR_TIMEOUT;
    }
}

/**
 * @brief Deinitialize WiFi manager
 */
void wifi_manager_deinit(void) {
    if (wifi_initialized) {
        wifi_manager_disconnect();
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
        esp_wifi_deinit();
        if (wifi_event_group != NULL) {
            vEventGroupDelete(wifi_event_group);
            wifi_event_group = NULL;
        }
        wifi_initialized = false;
        ESP_LOGI(TAG, "WiFi manager deinitialized");
    }
}
