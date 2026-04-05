/**
 * @file uart_handler.c
 * @brief Scanner C3 UART Handler - JSON Protocol Communication
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"

static const char *TAG = LOG_TAG_UART;
static QueueHandle_t uart_queue = NULL;
static bool uart_initialized = false;

/**
 * @brief Initialize UART with JSON protocol settings
 */
esp_err_t uart_handler_init(void) {
    if (uart_initialized) {
        ESP_LOGW(TAG, "UART already initialized");
        return ESP_OK;
    }

    // Configure UART parameters
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // Install UART driver
    esp_err_t ret = uart_driver_install(UART_PORT_NUM, UART_RX_BUFFER_SIZE,
                                        UART_TX_BUFFER_SIZE, UART_QUEUE_SIZE,
                                        &uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure UART parameters
    ret = uart_param_config(UART_PORT_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters: %s", esp_err_to_name(ret));
        uart_driver_delete(UART_PORT_NUM);
        return ret;
    }

    // Set UART pins
    ret = uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        uart_driver_delete(UART_PORT_NUM);
        return ret;
    }

    uart_initialized = true;
    ESP_LOGI(TAG, "UART initialized successfully (TX: GPIO%d, RX: GPIO%d, Baud: %d)",
             UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);

    return ESP_OK;
}

/**
 * @brief Send JSON message via UART
 * @param json_str JSON string to send (must NOT include newline)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_send_json(const char *json_str) {
    if (!uart_initialized) {
        ESP_LOGE(TAG, "UART not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (json_str == NULL) {
        ESP_LOGE(TAG, "NULL JSON string");
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(json_str);
    if (len == 0) {
        ESP_LOGE(TAG, "Empty JSON string");
        return ESP_ERR_INVALID_ARG;
    }

    // Send JSON string
    int written = uart_write_bytes(UART_PORT_NUM, json_str, len);
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write UART bytes");
        return ESP_FAIL;
    }

    // Send newline delimiter
    written = uart_write_bytes(UART_PORT_NUM, "\n", 1);
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write newline");
        return ESP_FAIL;
    }

    // Wait for transmission to complete
    uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(1000));

    ESP_LOGD(TAG, "Sent JSON (%d bytes): %.100s...", len, json_str);
    return ESP_OK;
}

/**
 * @brief Receive JSON message via UART (blocking with timeout)
 * @param buffer Buffer to store received JSON string
 * @param buffer_size Size of buffer
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes received (excluding newline), or -1 on error/timeout
 */
int uart_receive_json(char *buffer, size_t buffer_size, uint32_t timeout_ms) {
    if (!uart_initialized) {
        ESP_LOGE(TAG, "UART not initialized");
        return -1;
    }

    if (buffer == NULL || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid buffer");
        return -1;
    }

    size_t idx = 0;
    uint8_t byte;
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    // Read bytes until newline or timeout
    while (idx < buffer_size - 1) {
        // Check timeout
        if ((xTaskGetTickCount() - start_ticks) > timeout_ticks) {
            ESP_LOGW(TAG, "UART receive timeout after %d bytes", idx);
            return -1;
        }

        // Read one byte (non-blocking)
        int len = uart_read_bytes(UART_PORT_NUM, &byte, 1, pdMS_TO_TICKS(100));

        if (len < 0) {
            ESP_LOGE(TAG, "UART read error");
            return -1;
        }

        if (len == 0) {
            // No data available, continue waiting
            continue;
        }

        // Check for newline delimiter
        if (byte == '\n') {
            buffer[idx] = '\0';
            ESP_LOGD(TAG, "Received JSON (%d bytes): %.100s...", idx, buffer);
            return idx;
        }

        // Store byte
        buffer[idx++] = byte;
    }

    // Buffer overflow
    ESP_LOGE(TAG, "UART receive buffer overflow");
    buffer[buffer_size - 1] = '\0';
    return -1;
}

/**
 * @brief Parse received JSON and extract field
 * @param json_str JSON string to parse
 * @param field Field name to extract
 * @param value_out Output buffer for field value (must be freed by caller if type is string)
 * @param value_type Expected value type (cJSON_String, cJSON_Number, etc.)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_parse_json_field(const char *json_str, const char *field,
                                void *value_out, int value_type) {
    if (json_str == NULL || field == NULL || value_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *item = cJSON_GetObjectItem(root, field);
    if (item == NULL) {
        ESP_LOGE(TAG, "Field '%s' not found in JSON", field);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    if (item->type != value_type) {
        ESP_LOGE(TAG, "Field '%s' has wrong type (expected %d, got %d)",
                 field, value_type, item->type);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Extract value based on type
    switch (value_type) {
        case cJSON_String:
            *(char **)value_out = strdup(item->valuestring);
            break;

        case cJSON_Number:
            *(int *)value_out = item->valueint;
            break;

        case cJSON_True:
        case cJSON_False:
            *(bool *)value_out = cJSON_IsTrue(item);
            break;

        default:
            ESP_LOGE(TAG, "Unsupported JSON type: %d", value_type);
            cJSON_Delete(root);
            return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief Deinitialize UART
 */
void uart_handler_deinit(void) {
    if (uart_initialized) {
        uart_driver_delete(UART_PORT_NUM);
        uart_initialized = false;
        ESP_LOGI(TAG, "UART deinitialized");
    }
}
