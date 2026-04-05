#pragma once

#include "esp_err.h"

typedef enum {
    MULE_IDLE,          // Waiting for collection interval
    MULE_REQUESTING,    // Sent get_latest_req to miner
    MULE_RECEIVING,     // Receiving files from miner via UART
    MULE_DECODING,      // Base64 decoding received files into cache
    MULE_ERROR
} mule_state_t;

esp_err_t mule_task_init(void);
esp_err_t mule_task_start(void);
void mule_task_stop(void);
mule_state_t mule_task_get_state(void);
