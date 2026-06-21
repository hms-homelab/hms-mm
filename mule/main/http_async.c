/**
 * @file http_async.c
 * @brief Async worker pool for the mule HTTP server (see http_async.h).
 *
 * Canonical ESP-IDF async-handler pattern (examples/protocols/http_server/
 * async_handlers): N worker tasks pull request copies off a queue; a counting
 * semaphore tracks free workers. A blocking handler re-submits itself via
 * http_async_dispatch() so the single httpd task is freed to keep serving. When
 * all workers are busy, dispatch fails and the caller sends an immediate 503.
 */
#include "http_async.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "http_async";

/* Concurrency cap = number of worker tasks. The miner has ONE UART link and ONE
 * scanner task, so it can service exactly one request at a time. A single worker
 * matches that: it keeps the httpd task free (device stays responsive) while
 * serializing proxy/BLE work to what the miner can handle; excess concurrent
 * requests get an instant 503 instead of piling up. Each worker is a persistent
 * task whose stack comes from heap, so keep this small. */
#ifndef HTTP_ASYNC_WORKERS
#define HTTP_ASYNC_WORKERS    1
#endif
/* Workers run the same handlers the httpd task used to (cJSON + base64 + UART),
 * so match the old httpd stack_size (8192) to be safe. */
#define HTTP_ASYNC_STACK      8192
#define HTTP_ASYNC_PRIO       5

typedef struct {
    httpd_req_t        *req;
    http_req_handler_t  handler;
} async_req_t;

static QueueHandle_t     s_queue;
static SemaphoreHandle_t s_ready;      /* counting: number of free workers */
static TaskHandle_t      s_workers[HTTP_ASYNC_WORKERS];

bool http_async_on_worker(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < HTTP_ASYNC_WORKERS; i++)
        if (s_workers[i] == self) return true;
    return false;
}

esp_err_t http_async_dispatch(httpd_req_t *req, http_req_handler_t handler)
{
    if (!s_queue || !s_ready) return ESP_FAIL;   /* pool not up -> caller 503s */

    /* Claim a worker slot first (non-blocking) — if none free, reject now. */
    if (xSemaphoreTake(s_ready, 0) == pdFALSE) {
        ESP_LOGW(TAG, "no free worker (all %d busy) — rejecting", HTTP_ASYNC_WORKERS);
        return ESP_FAIL;
    }

    /* Get a copy of the request that survives past this handler returning. */
    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ready);
        ESP_LOGE(TAG, "async_handler_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    async_req_t job = { .req = copy, .handler = handler };
    if (xQueueSend(s_queue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_req_async_handler_complete(copy);
        xSemaphoreGive(s_ready);
        ESP_LOGE(TAG, "worker queue full");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void worker_task(void *arg)
{
    while (true) {
        async_req_t job;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) == pdTRUE) {
            job.handler(job.req);                         /* runs the real work */
            httpd_req_async_handler_complete(job.req);    /* free req + socket */
            xSemaphoreGive(s_ready);                       /* this worker is free again */
        }
    }
}

void http_async_init(void)
{
    s_ready = xSemaphoreCreateCounting(HTTP_ASYNC_WORKERS, 0);
    s_queue = xQueueCreate(HTTP_ASYNC_WORKERS, sizeof(async_req_t));
    if (!s_ready || !s_queue) {
        ESP_LOGE(TAG, "pool alloc failed — async disabled (handlers run sync)");
        return;
    }
    int started = 0;
    for (int i = 0; i < HTTP_ASYNC_WORKERS; i++) {
        if (xTaskCreate(worker_task, "http_async", HTTP_ASYNC_STACK, NULL,
                        HTTP_ASYNC_PRIO, &s_workers[i]) == pdPASS) {
            xSemaphoreGive(s_ready);   /* mark this worker available */
            started++;
        } else {
            ESP_LOGE(TAG, "worker %d create failed (low heap?)", i);
        }
    }
    ESP_LOGI(TAG, "async pool up: %d/%d workers (%d B stack each)",
             started, HTTP_ASYNC_WORKERS, HTTP_ASYNC_STACK);
}
