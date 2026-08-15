/**
 * @file file_server.c
 * @brief Mule HTTP server — proxies /dir and /download to miner via UART.
 *
 * No file caching. Each request goes through UART to the miner which
 * streams chunks from the ezShare card. Supports HTTP Range requests.
 */

#include "file_server.h"
#include "uart_handler.h"
#include "miner_link.h"
#include "ota_service.h"
#include "wifi_manager.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_rom_crc.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "file_srv";
static int s_req_id = 0;

#define MAX_PATH 256

void file_server_init(void)
{
    /* The link lock now lives in uart_handler (uart_link_lock), so that the
     * control endpoints can share it with these proxy handlers rather than
     * each subsystem guarding the miner with a private mutex of its own. */
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

/* Acknowledge a chunk, releasing the miner to send the next one.
 *
 * Sent only AFTER the chunk has been decoded, verified and handed to the HTTP
 * client, so it paces the miner to what this board can actually absorb rather
 * than to what the wire can carry. That distinction is the whole point: the
 * link has no hardware flow control, and a miner streaming faster than the
 * mule consumes simply loses bytes. */
static void send_chunk_ack(int req_id, int seq)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "chunk_ack");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON_AddNumberToObject(root, "seq", seq);
    char *json = cJSON_PrintUnformatted(root);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(root);
}

/* Tell the miner to stop streaming this req_id (HTTP client gone), then drain its
 * leftover chunks so the next request starts on a clean UART. Called while still
 * holding the link lock. The miner stops within a chunk or two of the abort; we
 * read and discard whatever was already queued until the link goes idle. */
static void proxy_send_abort_and_drain(int req_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "proxy_abort");
    cJSON_AddNumberToObject(root, "id", req_id);
    char *json = cJSON_PrintUnformatted(root);
    if (json) { uart_send_json(json); free(json); }
    cJSON_Delete(root);

    static char drain_buf[PROXY_UART_BUF_SIZE];
    int drained = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)PROXY_ABORT_DRAIN_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        /* short per-frame wait: a queued chunk reads fast; once the miner has
         * stopped and the link is idle, this times out and we're done. */
        if (uart_receive_json(drain_buf, sizeof(drain_buf), 200) <= 0) break;
        drained++;
    }
    ESP_LOGI(TAG, "proxy abort req_id=%d: drained %d stale frame(s)", req_id, drained);
}

/* ── Core proxy: forward request via UART, stream response ─────── */

static esp_err_t proxy_forward_request(httpd_req_t *req, const char *path,
                                        const char *content_type,
                                        uint32_t range_start, uint32_t range_end)
{
    esp_err_t ret = ESP_FAIL;
    bool chunked_started = false;
    /* Declared here, above the first `goto cleanup`: jumping over an
     * initialiser leaves the variable indeterminate, and cleanup reads these. */
    bool     meta_seen = false;
    uint32_t content_len = 0;
    uint32_t bytes_written = 0;
    /* Set on any mid-stream failure. The cleanup path then returns without
     * sending the terminating chunk, so the client sees a broken transfer
     * instead of a short file that looks whole. */
    bool     abandon_stream = false;

    /* Fast-fail, not a long block: handlers run on the single httpd task, so
     * waiting out someone else's download here would stall every other route
     * (including /api/status) for the duration. Second client gets 503 now. */
    if (!uart_link_lock(PROXY_LOCK_ACQUIRE_MS)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "5");
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

    /* static, not stack: 8 KB + 4 KB would overflow the 8 KB httpd task stack.
     * Safe — handlers are serialised by the link lock (one client at a time). */
    static char uart_buf[PROXY_UART_BUF_SIZE];
    static uint8_t decode_buf[PROXY_CHUNK_SIZE];
    int parse_failures = 0;
    int expected_seq = 0;
    /* No-progress stall window. The per-frame timeout resets on every successful
     * uart_receive_json (even stale-req_id frames), so it can't catch a miner
     * flooding stale frames after a client disconnect — the loop would spin
     * forever holding the link lock. This bounds total time WITHOUT a frame for
     * OUR req_id; it's reset only on a matching frame below. */
    int64_t stall_deadline = esp_timer_get_time() + (int64_t)PROXY_REQ_TIMEOUT_MS * 1000;

    while (true) {
        if (esp_timer_get_time() > stall_deadline) {
            ESP_LOGE(TAG, "proxy stalled (no progress) for req_id=%d — releasing", req_id);
            if (!chunked_started) {
                httpd_resp_set_status(req, "504 Gateway Timeout");
                httpd_resp_send(req, "Miner stalled", HTTPD_RESP_USE_STRLEN);
            } else {
                /* Do NOT terminate the stream. The 200 is already sent, so
                 * abandoning the connection mid-chunk is the only signal left
                 * that this is not a whole file. Closing it politely would
                 * hand the client a truncated recording it believes is
                 * complete. */
                abandon_stream = true;
            }
            ret = ESP_ERR_TIMEOUT;
            goto cleanup;
        }
        int len = uart_receive_json(uart_buf, sizeof(uart_buf), PROXY_REQ_TIMEOUT_MS);
        if (len <= 0) {
            ESP_LOGE(TAG, "UART timeout for req_id=%d", req_id);
            if (!chunked_started) {
                httpd_resp_set_status(req, "504 Gateway Timeout");
                httpd_resp_send(req, "Miner timeout", HTTPD_RESP_USE_STRLEN);
            } else {
                /* Do NOT terminate the stream. The 200 is already sent, so
                 * abandoning the connection mid-chunk is the only signal left
                 * that this is not a whole file. Closing it politely would
                 * hand the client a truncated recording it believes is
                 * complete. */
                abandon_stream = true;
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
                    abandon_stream = true;   /* see above: never finalise a partial stream */
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
        if (id_j && id_j->valueint == req_id)   /* progress for our request: extend the stall window */
            stall_deadline = esp_timer_get_time() + (int64_t)PROXY_REQ_TIMEOUT_MS * 1000;

        if (strcmp(type->valuestring, "error") == 0) {
            cJSON *em = cJSON_GetObjectItem(msg, "message");
            ESP_LOGE(TAG, "Miner error: %s", em ? em->valuestring : "unknown");

            /* Keep the miner's view of the ezShare link. Without it a failure
             * is an undifferentiated 502; with it, /api/status can say whether
             * the card was missing, the password was wrong, or the signal was
             * too weak. */
            cJSON *assoc_j  = cJSON_GetObjectItem(msg, "assoc");
            cJSON *rssi_j   = cJSON_GetObjectItem(msg, "rssi");
            cJSON *reason_j = cJSON_GetObjectItem(msg, "reason");
            if (cJSON_IsBool(assoc_j) || cJSON_IsNumber(reason_j))
                miner_link_note_diag(cJSON_IsTrue(assoc_j),
                                     cJSON_IsNumber(rssi_j)   ? rssi_j->valueint   : 0,
                                     cJSON_IsNumber(reason_j) ? reason_j->valueint : 0);

            if (!chunked_started) {
                httpd_resp_set_status(req, "502 Bad Gateway");
                httpd_resp_send(req, em ? em->valuestring : "Miner error",
                                HTTPD_RESP_USE_STRLEN);
            } else {
                /* Do NOT terminate the stream. The 200 is already sent, so
                 * abandoning the connection mid-chunk is the only signal left
                 * that this is not a whole file. Closing it politely would
                 * hand the client a truncated recording it believes is
                 * complete. */
                abandon_stream = true;
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
            if (http_status < 100 || http_status > 599) http_status = 200;
            uint32_t total_size = ts_j ? (uint32_t)ts_j->valueint : 0;
            uint32_t cl = cl_j ? (uint32_t)cl_j->valueint : 0;

            content_len = cl;
            meta_seen = true;
            ESP_LOGI(TAG, "META req_id=%d: status=%d content_len=%lu total=%lu",
                     req_id, http_status, (unsigned long)cl, (unsigned long)total_size);

            if (http_status == 206) {
                httpd_resp_set_status(req, "206 Partial Content");
                /* static, NOT stack: httpd_resp_set_hdr stores the POINTER and
                 * does not copy, and the headers are not written until the
                 * first chunk goes out — long after a local would have left
                 * scope. That is why Content-Range arrived empty. Safe as a
                 * static because the link lock serialises these handlers. */
                static char cr_hdr[64];
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

            cJSON *seq_j  = cJSON_GetObjectItem(msg, "seq");
            cJSON *d_j    = cJSON_GetObjectItem(msg, "d");
            cJSON *last_j = cJSON_GetObjectItem(msg, "last");
            cJSON *crc_j  = cJSON_GetObjectItem(msg, "c");
            bool is_last = last_j && cJSON_IsTrue(last_j);

            /* A gap in the sequence means a frame was lost, and every byte we
             * have already sent the client is now followed by a hole. We
             * cannot recover mid-response (the bytes are gone downstream), so
             * fail the transfer and let the client retry — previously this was
             * logged and ignored, which silently produced a corrupt file. */
            if (seq_j && seq_j->valueint != expected_seq) {
                ESP_LOGE(TAG, "Chunk seq gap: expected=%d got=%d — aborting transfer",
                         expected_seq, seq_j->valueint);
                cJSON_Delete(msg);
                if (!chunked_started) {
                    httpd_resp_set_status(req, "502 Bad Gateway");
                    httpd_resp_send(req, "Lost a chunk from the miner", HTTPD_RESP_USE_STRLEN);
                } else {
                    abandon_stream = true;   /* see above: never finalise a partial stream */
                }
                ret = ESP_FAIL;
                goto cleanup;
            }

            if (!d_j || !cJSON_IsString(d_j)) { cJSON_Delete(msg); continue; }

            /* Read the CRC out before freeing msg. Number, not int: a CRC
             * above 2^31 does not fit cJSON's valueint. */
            bool     have_crc = cJSON_IsNumber(crc_j);
            uint32_t want_crc = have_crc ? (uint32_t)crc_j->valuedouble : 0;

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
                    abandon_stream = true;   /* see above: never finalise a partial stream */
                }
                ret = ESP_FAIL;
                goto cleanup;
            }

            /* Base64 will happily decode a frame that lost bytes in transit
             * into shorter, well-formed garbage, so the decode succeeding
             * proves nothing about the payload. The CRC does. */
            if (have_crc) {
                uint32_t got_crc = esp_rom_crc32_le(0, decode_buf, decoded_len);
                if (got_crc != want_crc) {
                    ESP_LOGE(TAG, "Chunk CRC mismatch seq=%d (want %08lx got %08lx, %u B)",
                             expected_seq, (unsigned long)want_crc,
                             (unsigned long)got_crc, (unsigned)decoded_len);
                    if (!chunked_started) {
                        httpd_resp_set_status(req, "502 Bad Gateway");
                        httpd_resp_send(req, "Corrupt chunk from the miner", HTTPD_RESP_USE_STRLEN);
                    } else {
                        abandon_stream = true;   /* never finalise a corrupt stream */
                    }
                    ret = ESP_FAIL;
                    goto cleanup;
                }
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
            expected_seq++;
            bytes_written += decoded_len;

            /* Released only now, with the bytes already on their way to the
             * client. The miner blocks until this arrives. */
            if (!is_last) send_chunk_ack(req_id, expected_seq - 1);

            if (is_last) {
                /* The miner says that was the end. Believe it only if the byte
                 * count matches what the card promised. */
                if (meta_seen && content_len > 0 && bytes_written != content_len) {
                    ESP_LOGE(TAG, "INCOMPLETE req_id=%d: %lu of %lu bytes — "
                                  "failing the response rather than finalising it",
                             req_id, (unsigned long)bytes_written,
                             (unsigned long)content_len);
                    abandon_stream = true;
                    ret = ESP_FAIL;
                    goto cleanup;
                }
                httpd_resp_send_chunk(req, NULL, 0);
                ESP_LOGI(TAG, "Proxy complete req_id=%d: %lu bytes",
                         req_id, (unsigned long)bytes_written);
                ret = ESP_OK;
                goto cleanup;
            }
            continue;
        }

        cJSON_Delete(msg);
    }

cleanup:
    if (abandon_stream) {
        /* Returning non-OK makes esp_http_server drop the connection without
         * writing the zero-length terminator, so the client's HTTP layer
         * reports a truncated transfer. curl gives exit 18; a browser reports
         * a failed download. That is the whole point: a short recording must
         * not arrive looking complete. */
        ESP_LOGE(TAG, "ABANDONING req_id=%d after %lu bytes so the client sees a failure",
                 req_id, (unsigned long)bytes_written);
        if (ret == ESP_OK) ret = ESP_FAIL;
    }

    /* If a stream was in progress but didn't finish cleanly (client disconnect,
     * timeout, or error — ret is only ESP_OK on the is_last success path), the
     * miner may still be pushing chunks for this req_id. Tell it to stop and drain
     * the backlog so the next request starts on a clean UART. */
    if (chunked_started && ret != ESP_OK) {
        proxy_send_abort_and_drain(req_id);
    }
    uart_link_unlock();
    return ret;
}

/* ── O2Ring helpers ───────────────────────────────────────────── */

static esp_err_t send_o2ring_req(int req_id, const char *cmd, const char *filename)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "o2ring_req");
    cJSON_AddNumberToObject(root, "id", req_id);
    cJSON_AddStringToObject(root, "cmd", cmd);
    if (filename)
        cJSON_AddStringToObject(root, "name", filename);
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (json) { err = uart_send_json(json); free(json); }
    cJSON_Delete(root);
    return err;
}

static cJSON *wait_o2ring_json_response(httpd_req_t *req, int req_id,
                                         const char *expected_type,
                                         uint32_t timeout_ms)
{
    /* static: keep this 4 KB buffer off the 8 KB httpd task stack.
     * Callers (handle_o2ring_status/live) hold the link lock. */
    static char uart_buf[JSON_BUFFER_SIZE];
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        uint32_t remaining_ms = (uint32_t)((deadline - esp_timer_get_time()) / 1000);
        if (remaining_ms == 0) break;
        if (remaining_ms > timeout_ms) remaining_ms = timeout_ms;

        int len = uart_receive_json(uart_buf, sizeof(uart_buf), remaining_ms);
        if (len <= 0) continue;

        cJSON *msg = cJSON_Parse(uart_buf);
        if (!msg) continue;

        cJSON *id_j = cJSON_GetObjectItem(msg, "id");
        if (!id_j || id_j->valueint != req_id) { cJSON_Delete(msg); continue; }

        cJSON *type = cJSON_GetObjectItem(msg, "type");
        if (!type || !cJSON_IsString(type)) { cJSON_Delete(msg); continue; }

        if (strcmp(type->valuestring, "error") == 0) {
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_set_type(req, "application/json");
            char *err_json = cJSON_PrintUnformatted(msg);
            if (err_json) { httpd_resp_sendstr(req, err_json); free(err_json); }
            else httpd_resp_send(req, "{\"error\":\"unknown\"}", HTTPD_RESP_USE_STRLEN);
            cJSON_Delete(msg);
            return NULL;
        }

        if (strcmp(type->valuestring, expected_type) == 0) {
            return msg; // caller frees
        }

        cJSON_Delete(msg);
    }

    httpd_resp_set_status(req, "504 Gateway Timeout");
    httpd_resp_send(req, "O2Ring timeout", HTTPD_RESP_USE_STRLEN);
    return NULL;
}

/* ── O2Ring HTTP handlers ─────────────────────────────────────── */

static esp_err_t handle_o2ring_status(httpd_req_t *req)
{
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

    if (!uart_link_lock(PROXY_LOCK_ACQUIRE_MS)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Proxy busy", HTTPD_RESP_USE_STRLEN);
    }
    s_req_id++;
    int req_id = s_req_id;

    if (send_o2ring_req(req_id, "status", NULL) != ESP_OK) {
        uart_link_unlock();
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_send(req, "UART send failed", HTTPD_RESP_USE_STRLEN);
    }

    cJSON *resp = wait_o2ring_json_response(req, req_id, "o2ring_status", O2RING_STATUS_TIMEOUT_MS);
    if (resp) {
        httpd_resp_set_type(req, "application/json");
        cJSON_DeleteItemFromObject(resp, "type");
        cJSON_DeleteItemFromObject(resp, "id");
        char *json = cJSON_PrintUnformatted(resp);
        if (json) { httpd_resp_sendstr(req, json); free(json); }
        cJSON_Delete(resp);
    }
    uart_link_unlock();
    return ESP_OK;
}

static esp_err_t handle_o2ring_files(httpd_req_t *req)
{
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

    char query[512] = {0};
    char name_param[64] = {0};
    bool has_name = false;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "name", name_param, sizeof(name_param)) == ESP_OK && name_param[0])
            has_name = true;
    }

    /* Fast-fail on ACQUIRE only. Once held, the download may legitimately keep
     * the lock for O2RING_DOWNLOAD_TIMEOUT_MS — but a caller that finds it busy
     * must not park the httpd task for two minutes waiting its turn. */
    if (!uart_link_lock(PROXY_LOCK_ACQUIRE_MS)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Proxy busy", HTTPD_RESP_USE_STRLEN);
    }
    s_req_id++;
    int req_id = s_req_id;

    if (has_name) {
        /* Download mode — reuse proxy streaming */
        if (send_o2ring_req(req_id, "download", name_param) != ESP_OK) {
            uart_link_unlock();
            httpd_resp_set_status(req, "502 Bad Gateway");
            return httpd_resp_send(req, "UART send failed", HTTPD_RESP_USE_STRLEN);
        }

        char cd_hdr[128];
        snprintf(cd_hdr, sizeof(cd_hdr), "attachment; filename=\"%s\"", name_param);
        httpd_resp_set_hdr(req, "Content-Disposition", cd_hdr);

        /* Stream proxy_meta + proxy_chunks. static, not stack: 8 KB + 4 KB
         * would overflow the 8 KB httpd task; serialised by the link lock. */
        static char uart_buf[PROXY_UART_BUF_SIZE];
        static uint8_t decode_buf[PROXY_CHUNK_SIZE];
        bool chunked_started = false;
        bool abandon_stream = false;   /* never finalise a partial .vld */
        int expected_seq = 0;

        while (true) {
            int len = uart_receive_json(uart_buf, sizeof(uart_buf), O2RING_DOWNLOAD_TIMEOUT_MS);
            if (len <= 0) {
                if (!chunked_started) {
                    httpd_resp_set_status(req, "504 Gateway Timeout");
                    httpd_resp_send(req, "O2Ring download timeout", HTTPD_RESP_USE_STRLEN);
                } else {
                    abandon_stream = true;   /* see above: never finalise a partial stream */
                }
                break;
            }

            cJSON *msg = cJSON_Parse(uart_buf);
            if (!msg) continue;

            cJSON *type = cJSON_GetObjectItem(msg, "type");
            if (!type || !cJSON_IsString(type)) { cJSON_Delete(msg); continue; }

            cJSON *id_j = cJSON_GetObjectItem(msg, "id");
            if (id_j && id_j->valueint != req_id) { cJSON_Delete(msg); continue; }

            if (strcmp(type->valuestring, "error") == 0) {
                cJSON *em = cJSON_GetObjectItem(msg, "message");
                if (!chunked_started) {
                    httpd_resp_set_status(req, "502 Bad Gateway");
                    httpd_resp_send(req, em ? em->valuestring : "Download error", HTTPD_RESP_USE_STRLEN);
                } else {
                    abandon_stream = true;   /* see above: never finalise a partial stream */
                }
                cJSON_Delete(msg);
                break;
            }

            if (strcmp(type->valuestring, "proxy_meta") == 0) {
                cJSON_Delete(msg);
                continue;
            }

            if (strcmp(type->valuestring, "proxy_chunk") == 0) {
                cJSON *d_j = cJSON_GetObjectItem(msg, "d");
                cJSON *last_j = cJSON_GetObjectItem(msg, "last");
                cJSON *seq_j = cJSON_GetObjectItem(msg, "seq");
                cJSON *crc_j = cJSON_GetObjectItem(msg, "c");
                bool is_last = last_j && cJSON_IsTrue(last_j);

                if (!d_j || !cJSON_IsString(d_j)) { cJSON_Delete(msg); continue; }

                /* Same integrity rules as the ezShare path. This loop used to
                 * ignore seq entirely and `continue` past a failed decode,
                 * which quietly dropped a chunk and handed the client a
                 * truncated .vld that still looked like a successful 200. */
                bool seq_gap = seq_j && seq_j->valueint != expected_seq;
                bool     have_crc = cJSON_IsNumber(crc_j);
                uint32_t want_crc = have_crc ? (uint32_t)crc_j->valuedouble : 0;

                size_t decoded_len = 0;
                int rc = mbedtls_base64_decode(decode_buf, sizeof(decode_buf), &decoded_len,
                                                (const unsigned char *)d_j->valuestring,
                                                strlen(d_j->valuestring));
                cJSON_Delete(msg);

                const char *bad = NULL;
                if (seq_gap)      bad = "chunk sequence gap";
                else if (rc != 0) bad = "base64 decode failed";
                else if (have_crc && esp_rom_crc32_le(0, decode_buf, decoded_len) != want_crc)
                    bad = "chunk CRC mismatch";

                if (bad) {
                    ESP_LOGE(TAG, "O2Ring download req_id=%d aborted: %s (seq=%d)",
                             req_id, bad, expected_seq);
                    if (chunked_started) {
                        /* Abandon rather than terminate: a clean end here would
                         * present a partial .vld as a complete one. */
                        abandon_stream = true;
                    } else {
                        httpd_resp_set_status(req, "502 Bad Gateway");
                        httpd_resp_send(req, bad, HTTPD_RESP_USE_STRLEN);
                    }
                    break;
                }

                if (!chunked_started) {
                    httpd_resp_set_type(req, "application/octet-stream");
                    chunked_started = true;
                }

                if (httpd_resp_send_chunk(req, (const char *)decode_buf, decoded_len) != ESP_OK) {
                    ESP_LOGW(TAG, "HTTP client disconnected during O2Ring download");
                    abandon_stream = true;
                    break;
                }
                expected_seq++;

                if (is_last) {
                    httpd_resp_send_chunk(req, NULL, 0);
                    break;
                }
                continue;
            }
            cJSON_Delete(msg);
        }

        if (abandon_stream) {
            /* Same rule as the ezShare path: leave the chunked response
             * unterminated so the client's HTTP layer reports a truncated
             * download rather than accepting a partial .vld as whole. */
            ESP_LOGE(TAG, "ABANDONING O2Ring download req_id=%d so the client sees a failure",
                     req_id);
            uart_link_unlock();
            return ESP_FAIL;
        }
    } else {
        /* List mode */
        if (send_o2ring_req(req_id, "files", NULL) != ESP_OK) {
            uart_link_unlock();
            httpd_resp_set_status(req, "502 Bad Gateway");
            return httpd_resp_send(req, "UART send failed", HTTPD_RESP_USE_STRLEN);
        }

        cJSON *resp = wait_o2ring_json_response(req, req_id, "o2ring_files", O2RING_FILES_TIMEOUT_MS);
        if (resp) {
            httpd_resp_set_type(req, "application/json");
            cJSON_DeleteItemFromObject(resp, "type");
            cJSON_DeleteItemFromObject(resp, "id");
            char *json = cJSON_PrintUnformatted(resp);
            if (json) { httpd_resp_sendstr(req, json); free(json); }
            cJSON_Delete(resp);
        }
    }

    uart_link_unlock();
    return ESP_OK;
}

static esp_err_t handle_o2ring_live(httpd_req_t *req)
{
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

    if (!uart_link_lock(PROXY_LOCK_ACQUIRE_MS)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Proxy busy", HTTPD_RESP_USE_STRLEN);
    }
    s_req_id++;
    int req_id = s_req_id;

    if (send_o2ring_req(req_id, "live", NULL) != ESP_OK) {
        uart_link_unlock();
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_send(req, "UART send failed", HTTPD_RESP_USE_STRLEN);
    }

    cJSON *resp = wait_o2ring_json_response(req, req_id, "o2ring_live", O2RING_LIVE_TIMEOUT_MS);
    if (resp) {
        httpd_resp_set_type(req, "application/json");
        cJSON_DeleteItemFromObject(resp, "type");
        cJSON_DeleteItemFromObject(resp, "id");
        char *json = cJSON_PrintUnformatted(resp);
        if (json) { httpd_resp_sendstr(req, json); free(json); }
        cJSON_Delete(resp);
    }
    uart_link_unlock();
    return ESP_OK;
}

/* ── HTTP handlers ─────────────────────────────────────────────── */

static esp_err_t handle_dir(httpd_req_t *req)
{
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

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
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

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

void file_server_register(httpd_handle_t server)
{
    httpd_uri_t uris[] = {
        { .uri = "/dir",            .method = HTTP_GET, .handler = handle_dir },
        { .uri = "/download",       .method = HTTP_GET, .handler = handle_download },
        { .uri = "/o2ring/status",  .method = HTTP_GET, .handler = handle_o2ring_status },
        { .uri = "/o2ring/files",   .method = HTTP_GET, .handler = handle_o2ring_files },
        { .uri = "/o2ring/live",    .method = HTTP_GET, .handler = handle_o2ring_live },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);
    ESP_LOGI(TAG, "Registered: /dir /download and the o2ring routes");
}
