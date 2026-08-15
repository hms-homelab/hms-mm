#pragma once

/**
 * @file control_server.h
 * @brief The mule's local HTTP server: device page, status, and admin actions.
 *
 * Owns the single httpd on port 80 and registers the endpoints that are not
 * about moving data:
 *
 *   GET  /             device page
 *   GET  /api/status   health, identity, and the miner's ezShare diagnostics
 *   POST /api/reboot   restart the mule, or the miner, or both
 *   POST /api/reset    forget the home WiFi and return to the setup portal
 *   POST /api/config   write credentials, or toggle the O2 Ring
 *   GET  /api/logs     recent log lines (log_ring.c)
 *
 * The data routes (/dir, /download and the o2ring endpoints) are mounted onto the same
 * server by file_server_register(), so everything shares one httpd and one
 * link lock rather than competing for the miner.
 */

#include "esp_err.h"
#include "esp_http_server.h"

/** Start the server and register every endpoint. */
esp_err_t control_server_start(void);

/** The running server handle, or NULL. */
httpd_handle_t control_server_get_handle(void);

void control_server_stop(void);
