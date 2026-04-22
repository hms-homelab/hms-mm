#ifndef SCANNER_TASK_H
#define SCANNER_TASK_H

#include "esp_err.h"

typedef enum {
    SCANNER_IDLE,           // Waiting for UART proxy_req or set_config
    SCANNER_PROXY,          // Connected to ezShare, streaming response
    SCANNER_ERROR           // Error state (retry)
} scanner_state_t;

esp_err_t scanner_task_init(void);
esp_err_t scanner_task_start(void);
void scanner_task_stop(void);
scanner_state_t scanner_task_get_state(void);

#endif // SCANNER_TASK_H
