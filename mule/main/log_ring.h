#pragma once

/**
 * @file log_ring.h
 * @brief In-memory ring of recent log lines, served over HTTP.
 *
 * The only way to see what this device is doing has been a USB cable. Once a
 * unit is mounted behind a CPAP machine that is not a realistic thing to ask
 * for, so a bounded ring of recent lines is kept in RAM and exposed at
 * /api/logs. The console output is untouched.
 */

#include "esp_err.h"
#include "esp_http_server.h"
#include <stddef.h>

/**
 * @brief Start capturing ESP_LOG output.
 *
 * Call as early in app_main as possible, before anything worth reading has
 * been logged. Installs a vprintf hook that copies each line into the ring and
 * then forwards to the original handler, so the serial console is unaffected.
 */
void log_ring_init(void);

/**
 * @brief Copy the ring into out, oldest line first, NUL-terminated.
 *
 * @param tail_lines  if > 0, return only the last N lines.
 * @return bytes written, excluding the terminator.
 */
size_t log_ring_snapshot(char *out, size_t out_size, int tail_lines);

/** Register GET /api/logs (supports ?n=<lines>). */
esp_err_t log_ring_register(httpd_handle_t server);
