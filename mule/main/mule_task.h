#pragma once

#include "esp_err.h"

typedef enum {
    MULE_IDLE
} mule_state_t;

esp_err_t mule_task_init(void);
esp_err_t mule_task_start(void);
void mule_task_stop(void);
mule_state_t mule_task_get_state(void);
