/**
 * @file uart_handler.h
 * @brief Scanner C3 UART Handler - JSON Protocol Communication
 */

#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialize UART with JSON protocol settings
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_handler_init(void);

/**
 * @brief Send JSON message via UART
 * @param json_str JSON string to send (must NOT include newline)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_send_json(const char *json_str);

/**
 * @brief Receive JSON message via UART (blocking with timeout)
 * @param buffer Buffer to store received JSON string
 * @param buffer_size Size of buffer
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes received (excluding newline), or -1 on error/timeout
 */
int uart_receive_json(char *buffer, size_t buffer_size, uint32_t timeout_ms);

/**
 * @brief Parse received JSON and extract field
 * @param json_str JSON string to parse
 * @param field Field name to extract
 * @param value_out Output buffer for field value (must be freed by caller if type is string)
 * @param value_type Expected value type (cJSON_String, cJSON_Number, etc.)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_parse_json_field(const char *json_str, const char *field,
                                void *value_out, int value_type);

/**
 * @brief Deinitialize UART
 */
void uart_handler_deinit(void);

#endif // UART_HANDLER_H
