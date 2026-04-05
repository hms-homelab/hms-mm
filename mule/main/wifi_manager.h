#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_ERROR
} wifi_status_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms);
esp_err_t wifi_manager_disconnect(void);
bool wifi_manager_is_connected(void);
void wifi_manager_deinit(void);
