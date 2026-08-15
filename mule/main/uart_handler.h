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
 * @brief Hold the link across a whole request/response exchange.
 *
 * The per-frame TX mutex only protects one write. A conversation is several
 * frames, and two callers interleaving would steal each other's replies — the
 * miner answers whoever reads next, not whoever asked. Everything that talks
 * to the miner (the HTTP proxy handlers, the O2Ring endpoints, and the control
 * endpoints added later) takes this for the duration of the exchange.
 *
 * @param timeout_ms  0 waits forever.
 * @return true if acquired.
 */
bool uart_link_lock(uint32_t timeout_ms);
void uart_link_unlock(void);

/**
 * @brief Discard any partially-received frame and the driver's RX backlog.
 *
 * Call after abandoning a request so the next one does not begin by parsing
 * the tail of the previous response.
 */
void uart_rx_flush(void);

/**
 * @brief Deinitialize UART
 */
void uart_handler_deinit(void);

#endif // UART_HANDLER_H
