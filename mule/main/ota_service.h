#pragma once

/**
 * @file ota_service.h
 * @brief Firmware update endpoints and the lockout that protects them.
 *
 *   GET  /api/update          upload page
 *   POST /api/update/mule     raw .bin in the body, no network needed
 *   POST /api/update/miner    same, relayed over the link
 *   POST /api/ota             {"target":"mule"|"miner","url":"..."} pull
 *   POST /api/cancel_update   abandon a URL pull in progress
 *
 * Nothing here polls for updates. The device contacts a server only when you
 * hand it a URL, which is the whole difference between this and the cloud
 * project it was ported from.
 */

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OTA_IDLE = 0,
    OTA_RUNNING,
    OTA_FINALIZING,
} ota_state_t;

typedef struct {
    ota_state_t state;
    const char *state_str;
    const char *target;      /* "mule" or "miner" while running */
    uint32_t    written;
    uint32_t    total;       /* 0 when the server did not give a length */
    const char *error;       /* last failure, or NULL */
} ota_status_t;

void ota_service_get_status(ota_status_t *out);

/**
 * @brief Reject a request while an update is in flight.
 *
 * An update rewrites flash and holds the link, so anything that would touch
 * either has to stand aside. Returns true if the request was answered with a
 * 503, in which case the caller must stop.
 *
 * Applied to every route that drives the miner or mutates config. Deliberately
 * NOT applied to /api/status, /api/logs or /api/cancel_update: those are how
 * you watch an update and how you stop one.
 */
bool ota_service_reject_if_busy(httpd_req_t *req);

/** Mark the running image valid once the device has proved itself. */
void ota_service_confirm_boot(void);

esp_err_t ota_service_register(httpd_handle_t server);
