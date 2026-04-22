/**
 * @file file_server.c
 * @brief Mule HTTP server — proxies /dir and /download to miner via UART.
 *
 * No file caching. Each request goes through UART to the miner which
 * streams chunks from the ezShare card. Supports HTTP Range requests.
 */

#include "file_server.h"
#include "uart_handler.h"
#include "wifi_manager.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "file_srv";
static SemaphoreHandle_t s_proxy_mutex = NULL;
static int s_req_id = 0;

#define MAX_PATH 256

void file_server_init(void)
{
    if (!s_proxy_mutex)
        s_proxy_mutex = xSemaphoreCreateMutex();
}

/* ── Range header parsing ──────────────────────────────────────── */

static bool parse_range_header(httpd_req_t *req, uint32_t *start, uint32_t *end)
{
    *start = 0;
    *end = 0;
    char buf[64];
    if (httpd_req_get_hdr_value_str(req, "Range", buf, sizeof(buf)) != ESP_OK)
        return false;
    if (strncmp(buf, "bytes=", 6) != 0) return false;
    const char *p = buf + 6;
    if (*p == '-') return false;

    char *endptr = NULL;
    unsigned long s = strtoul(p, &endptr, 10);
    if (endptr == p) return false;
    *start = (uint32_t)s;

    if (*endptr == '-' && *(endptr + 1) >= '0' && *(endptr + 1) <= '9') {
        unsigned long e = strtoul(endptr + 1, NULL, 10);
        *end = (uint32_t)e;
    }

    ESP_LOGI(TAG, "Range: bytes=%lu-%lu", (unsigned long)*start, (unsigned long)*end);
    return true;
}

/* ── Send proxy_req via UART ───────────────────────────────────── */

static esp_err_t send_proxy_req(int req_id, const char *path,
                                 uint32_t range_start, uint32_t range_end)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "proxy_req");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddNumberToObject(root, "rs", range_start);
    cJSON_AddNumberToObject(root, "re", range_end);

    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (json) { err = uart_send_json(json); free(json); }
    cJSON_Delete(root);
    return err;
}

/* ── Core proxy: forward request via UART, stream response ─────── */

static esp_err_t proxy_forward_request(httpd_req_t *req, const char *path,
                                        const char *content_type,
                                        uint32_t range_start, uint32_t range_end)
{
    esp_err_t ret = ESP_FAIL;
    bool chunked_started = false;

    if (xSemaphoreTake(s_proxy_mutex, pdMS_TO_TICKS(PROXY_REQ_TIMEOUT_MS + 5000)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Proxy busy", HTTPD_RESP_USE_STRLEN);
        return ESP_ERR_INVALID_STATE;
    }

    s_req_id++;
    int req_id = s_req_id;

    ESP_LOGI(TAG, "proxy req_id=%d path=%s range=%lu-%lu",
             req_id, path, (unsigned long)range_start, (unsigned long)range_end);

    if (send_proxy_req(req_id, path, range_start, range_end) != ESP_OK) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_send(req, "UART send failed", HTTPD_RESP_USE_STRLEN);
        ret = ESP_FAIL;
        goto cleanup;
    }

    char uart_buf[PROXY_UART_BUF_SIZE];
    uint8_t decode_buf[PROXY_CHUNK_SIZE];
    int parse_failures = 0;

    while (true) {
        int len = uart_receive_json(uart_buf, sizeof(uart_buf), PROXY_REQ_TIMEOUT_MS);
        if (len <= 0) {
            ESP_LOGE(TAG, "UART timeout for req_id=%d", req_id);
            if (!chunked_started) {
                httpd_resp_set_status(req, "504 Gateway Timeout");
                httpd_resp_send(req, "Miner timeout", HTTPD_RESP_USE_STRLEN);
            } else {
                httpd_resp_send_chunk(req, NULL, 0);
            }
            ret = ESP_ERR_TIMEOUT;
            goto cleanup;
        }

        cJSON *msg = cJSON_Parse(uart_buf);
        if (!msg) {
            if (++parse_failures > 10) {
                ESP_LOGE(TAG, "Too many parse failures for req_id=%d", req_id);
                if (!chunked_started) {
                    httpd_resp_set_status(req, "502 Bad Gateway");
                    httpd_resp_send(req, "Protocol error", HTTPD_RESP_USE_STRLEN);
                } else {
                    httpd_resp_send_chunk(req, NULL, 0);
                }
                ret = ESP_FAIL;
                goto cleanup;
            }
            continue;
        }
        parse_failures = 0;

        cJSON *type = cJSON_GetObjectItem(msg, "type");
        if (!type || !cJSON_IsString(type)) { cJSON_Delete(msg); continue; }

        cJSON *id_j = cJSON_GetObjectItem(msg, "id");
        if (id_j && id_j->valueint != req_id) { cJSON_Delete(msg); continue; }

        if (strcmp(type->valuestring, "error") == 0) {
            cJSON *em = cJSON_GetObjectItem(msg, "message");
            ESP_LOGE(TAG, "Miner error: %s", em ? em->valuestring : "unknown");
            if (!chunked_started) {
                httpd_resp_set_status(req, "502 Bad Gateway");
                httpd_resp_send(req, em ? em->valuestring : "Miner error",
                                HTTPD_RESP_USE_STRLEN);
            } else {
                httpd_resp_send_chunk(req, NULL, 0);
            }
            cJSON_Delete(msg);
            ret = ESP_FAIL;
            goto cleanup;
        }

        if (strcmp(type->valuestring, "proxy_meta") == 0) {
            cJSON *meta_id = cJSON_GetObjectItem(msg, "id");
            if (meta_id && meta_id->valueint != req_id) { cJSON_Delete(msg); continue; }

            cJSON *st_j = cJSON_GetObjectItem(msg, "st");
            cJSON *cl_j = cJSON_GetObjectItem(msg, "cl");
            cJSON *ts_j = cJSON_GetObjectItem(msg, "ts");

            int http_status = st_j ? st_j->valueint : 200;
            uint32_t total_size = ts_j ? (uint32_t)ts_j->valueint : 0;
            uint32_t cl = cl_j ? (uint32_t)cl_j->valueint : 0;

            if (http_status == 206) {
                httpd_resp_set_status(req, "206 Partial Content");
                char cr_hdr[64];
                uint32_t end_byte = range_end > 0 ? range_end
                    : (range_start + cl - 1);
                if (total_size > 0)
                    snprintf(cr_hdr, sizeof(cr_hdr), "bytes %lu-%lu/%lu",
                             (unsigned long)range_start, (unsigned long)end_byte,
                             (unsigned long)total_size);
                else
                    snprintf(cr_hdr, sizeof(cr_hdr), "bytes %lu-%lu/*",
                             (unsigned long)range_start, (unsigned long)end_byte);
                httpd_resp_set_hdr(req, "Content-Range", cr_hdr);
                ESP_LOGI(TAG, "META: 206, Content-Range: %s", cr_hdr);
            }
            httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
            cJSON_Delete(msg);
            continue;
        }

        if (strcmp(type->valuestring, "proxy_chunk") == 0) {
            cJSON *chunk_id = cJSON_GetObjectItem(msg, "id");
            if (chunk_id && chunk_id->valueint != req_id) { cJSON_Delete(msg); continue; }

            cJSON *d_j    = cJSON_GetObjectItem(msg, "d");
            cJSON *last_j = cJSON_GetObjectItem(msg, "last");
            bool is_last = last_j && cJSON_IsTrue(last_j);

            if (!d_j || !cJSON_IsString(d_j)) { cJSON_Delete(msg); continue; }

            size_t decoded_len = 0;
            int rc = mbedtls_base64_decode(decode_buf, sizeof(decode_buf), &decoded_len,
                                            (const unsigned char *)d_j->valuestring,
                                            strlen(d_j->valuestring));
            cJSON_Delete(msg);

            if (rc != 0) {
                ESP_LOGE(TAG, "base64 decode failed");
                if (!chunked_started) {
                    httpd_resp_set_status(req, "502 Bad Gateway");
                    httpd_resp_send(req, "Decode error", HTTPD_RESP_USE_STRLEN);
                } else {
                    httpd_resp_send_chunk(req, NULL, 0);
                }
                ret = ESP_FAIL;
                goto cleanup;
            }

            if (!chunked_started) {
                httpd_resp_set_type(req, content_type);
                chunked_started = true;
            }

            esp_err_t chunk_rc = httpd_resp_send_chunk(req, (const char *)decode_buf, decoded_len);
            if (chunk_rc != ESP_OK) {
                ESP_LOGW(TAG, "HTTP client disconnected");
                ret = ESP_FAIL;
                goto cleanup;
            }

            if (is_last) {
                httpd_resp_send_chunk(req, NULL, 0);
                ESP_LOGI(TAG, "Proxy complete req_id=%d", req_id);
                ret = ESP_OK;
                goto cleanup;
            }
            continue;
        }

        cJSON_Delete(msg);
    }

cleanup:
    xSemaphoreGive(s_proxy_mutex);
    return ret;
}

/* ── HTTP handlers ─────────────────────────────────────────────── */

static esp_err_t handle_dir(httpd_req_t *req)
{
    char query[MAX_PATH * 2] = {0};
    char dir_param[MAX_PATH] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        httpd_query_key_value(query, "dir", dir_param, sizeof(dir_param));

    if (dir_param[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dir param");
        return ESP_OK;
    }

    char path[MAX_PATH + 16];
    snprintf(path, sizeof(path), "/dir?dir=%s", dir_param);

    return proxy_forward_request(req, path, "text/html", 0, 0);
}

static esp_err_t handle_download(httpd_req_t *req)
{
    char query[MAX_PATH * 2] = {0};
    char file_param[MAX_PATH] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing file param");
        return ESP_OK;
    }

    char path[MAX_PATH + 32];
    snprintf(path, sizeof(path), "/download?file=%s", file_param);

    uint32_t range_start = 0, range_end = 0;
    parse_range_header(req, &range_start, &range_end);

    return proxy_forward_request(req, path, "application/octet-stream",
                                  range_start, range_end);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    int64_t up = esp_timer_get_time() / 1000000LL;
    int secs = (int)(up % 60), mins = (int)((up / 60) % 60), hrs = (int)(up / 3600);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"proxy\",\"wifi\":%s,\"mqtt\":false,"
             "\"uptime\":\"%02d:%02d:%02d\"}",
             wifi_manager_is_connected() ? "true" : "false",
             hrs, mins, secs);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

void file_server_register(httpd_handle_t server)
{
    httpd_uri_t uris[] = {
        { .uri = "/dir",        .method = HTTP_GET, .handler = handle_dir },
        { .uri = "/download",   .method = HTTP_GET, .handler = handle_download },
        { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status },
    };
    for (int i = 0; i < 3; i++) httpd_register_uri_handler(server, &uris[i]);
    ESP_LOGI(TAG, "Registered: /dir /download /api/status (proxy mode)");
}
