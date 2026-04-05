/**
 * @file wifi_manager.h
 * @brief Scanner C3 WiFi Manager - ez Share WiFi Connection
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief WiFi connection status
 */
typedef enum {
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_ERROR
} wifi_status_t;

/**
 * @brief Initialize WiFi manager
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Connect to WiFi network
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @param timeout_ms Connection timeout in milliseconds
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * @brief Disconnect from WiFi network
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Get current WiFi connection status
 * @return Current WiFi status
 */
wifi_status_t wifi_manager_get_status(void);

/**
 * @brief Check if WiFi is connected
 * @return true if connected, false otherwise
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Wait for WiFi connection (blocking)
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT if timeout, error code otherwise
 */
esp_err_t wifi_manager_wait_connection(uint32_t timeout_ms);

/**
 * @brief Deinitialize WiFi manager
 */
void wifi_manager_deinit(void);

#endif // WIFI_MANAGER_H
