/**
 * @file http_async.h
 * @brief Async worker pool for the mule's HTTP server.
 *
 * esp_http_server runs a SINGLE task: it processes one URI handler at a time,
 * synchronously, in that task. So any handler that blocks (a /dir or /download
 * that waits on the UART round-trip to the miner, or an o2ring request that
 * waits up to ~2 min on a BLE op) wedges the whole server — every other request,
 * even the static /api/status, stalls until it returns.
 *
 * This pool fixes that: a blocking handler re-submits itself to a worker task,
 * freeing the httpd task to keep accepting and answering requests. The number of
 * workers is the real concurrency cap — when all are busy a new request gets an
 * immediate 503. The miner has ONE UART link and ONE scanner, so it services one
 * request at a time; a single worker matches that and keeps heap small, while the
 * httpd task stays responsive.
 *
 * Usage in a (re-entrant) handler:
 *     static esp_err_t handle_x(httpd_req_t *req) {
 *         if (!http_async_on_worker()) {          // first call, on httpd task
 *             if (http_async_dispatch(req, handle_x) == ESP_OK) return ESP_OK;
 *             httpd_resp_set_status(req, "503 Service Unavailable");
 *             httpd_resp_send(req, "Server busy", HTTPD_RESP_USE_STRLEN);
 *             return ESP_OK;
 *         }
 *         ... real blocking work runs here, on a worker task ...
 *     }
 */
#ifndef HTTP_ASYNC_H
#define HTTP_ASYNC_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* A plain URI handler function (same shape as httpd_uri_t.handler). */
typedef esp_err_t (*http_req_handler_t)(httpd_req_t *req);

/* Create the worker pool (counting semaphore + queue + worker tasks).
 * Call once at startup before the HTTP server starts serving. */
void http_async_init(void);

/* True if the caller is already running on a worker task (i.e. this is the
 * second, resubmitted invocation of a handler — time to do the real work). */
bool http_async_on_worker(void);

/* Submit a request to a worker task. On ESP_OK the handler will be re-invoked on
 * a worker (caller must return ESP_OK immediately, doing nothing else with req).
 * On failure (no free worker / pool down) the caller should send a 503. */
esp_err_t http_async_dispatch(httpd_req_t *req, http_req_handler_t handler);

#endif /* HTTP_ASYNC_H */
