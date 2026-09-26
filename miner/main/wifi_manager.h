/**
 * @file wifi_manager.h
 * @brief Miner WiFi manager: the ezShare link, started once and held.
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
 * @brief Start the ezShare link and hold it. Non-blocking.
 *
 * The first call starts the station; every failed join and every drop after
 * that is retried in the background (ez_retry.h) forever. Later calls change
 * nothing. Callers read wifi_manager_is_connected() for the answer and never
 * wait for a join.
 */
esp_err_t wifi_manager_start(const char *ssid, const char *password);

/** Called on the WiFi event task when the link comes up (true) or drops. */
typedef void (*wifi_link_cb_t)(bool up);
void wifi_manager_set_link_cb(wifi_link_cb_t cb);

/**
 * @brief Turn WiFi power save off (WIFI_PS_NONE) from the next start.
 *
 * The default modem sleep parks the station between DTIM beacons: every TCP
 * round trip waits for the next one, and the ezShare's buffering for a
 * sleeping station drops unicast, so transfers stall on a link that looks
 * healthy. But ESP-IDF requires modem sleep while Bluetooth is enabled, so
 * the scanner only turns it off when the ring is disabled.
 */
void wifi_manager_set_power_save(bool off);

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
 * @brief Last raw WIFI_REASON_* code from the driver (0 if never disconnected).
 *
 * Reported to the mule with proxy errors. 201 (NO_AP_FOUND) means the card is
 * off, asleep or out of range; 202 (AUTH_FAIL) means the password is wrong.
 * Both otherwise surface as an indistinguishable 502.
 */
int wifi_manager_last_disc_reason(void);

/**
 * @brief Current AP RSSI in dBm, or 0 when not associated.
 */
int wifi_manager_rssi(void);

/** @brief Stop the link and free the driver. */
void wifi_manager_deinit(void);

#endif // WIFI_MANAGER_H
