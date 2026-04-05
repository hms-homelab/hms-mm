/**
 * @file scanner_task.h
 * @brief Scanner C3 State Machine Task
 */

#ifndef SCANNER_TASK_H
#define SCANNER_TASK_H

#include "esp_err.h"

/**
 * @brief Scanner state machine states
 */
typedef enum {
    SCANNER_IDLE,           // Waiting for timestamp request
    SCANNER_CONNECTING,     // Connecting to ez Share WiFi
    SCANNER_LISTING,        // HTTP directory listing
    SCANNER_DOWNLOADING,    // Downloading EDF files
    SCANNER_SENDING,        // Sending files via UART
    SCANNER_DISCONNECTING,  // Disconnecting from ez Share
    SCANNER_ERROR           // Error state (retry)
} scanner_state_t;

/**
 * @brief Initialize scanner task
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t scanner_task_init(void);

/**
 * @brief Start scanner task
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t scanner_task_start(void);

/**
 * @brief Stop scanner task
 */
void scanner_task_stop(void);

/**
 * @brief Get current scanner state
 * @return Current state
 */
scanner_state_t scanner_task_get_state(void);

#endif // SCANNER_TASK_H
