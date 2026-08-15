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
/**
 * @brief Last raw WIFI_REASON_* from the driver (0 if never disconnected).
 *
 * Lets the boot path tell "these credentials are wrong" apart from "the router
 * is not back yet", which need very different amounts of patience.
 */
int wifi_manager_last_disc_reason(void);

/**
 * @brief True if the last failure looks like bad credentials rather than an
 *        absent network.
 *
 * Deliberately fuzzy: a marginal RF link also produces AUTH_EXPIRE with a
 * perfectly good password (the station is heard intermittently), so this
 * shortens the retry budget rather than deciding on one attempt.
 */
bool wifi_manager_last_failure_was_auth(void);

void wifi_manager_deinit(void);
