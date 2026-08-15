/**
 * @file log_ring.c
 * @brief In-memory ring of recent log lines (see log_ring.h).
 */

#include "log_ring.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 8 KB holds a few hundred lines, which in practice covers a boot plus
 * whatever went wrong afterwards. Larger would be nicer and the C3 does not
 * have the heap to spare. */
#define LOG_RING_SIZE   8192
/* Longest single line captured. Anything past this is truncated into the ring
 * but still reaches the console in full. */
#define LOG_LINE_MAX    200

static char   s_ring[LOG_RING_SIZE];
static size_t s_head;        /* next write position */
static bool   s_wrapped;     /* ring has overwritten at least once */
static vprintf_like_t s_prev;

/* The hook runs on whatever task is logging, so appends must be atomic against
 * each other. A spinlock (not a mutex) because this can be reached from
 * contexts where blocking is not allowed, and the critical section is only a
 * couple of memcpys. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void ring_append(const char *data, size_t len)
{
    if (len == 0) return;
    if (len > LOG_RING_SIZE) {          /* keep the tail of an oversized line */
        data += len - LOG_RING_SIZE;
        len = LOG_RING_SIZE;
    }

    portENTER_CRITICAL(&s_mux);
    size_t first = LOG_RING_SIZE - s_head;
    if (first > len) first = len;
    memcpy(s_ring + s_head, data, first);
    if (len > first) {                  /* wrapped */
        memcpy(s_ring, data + first, len - first);
        s_wrapped = true;
        s_head = len - first;
    } else {
        s_head += first;
        if (s_head == LOG_RING_SIZE) { s_head = 0; s_wrapped = true; }
    }
    portEXIT_CRITICAL(&s_mux);
}

/**
 * Copy src to dst dropping ANSI CSI escapes.
 *
 * ESP_LOG colourises by level, so the raw stream is full of "\033[0;32m".
 * Those are right for a terminal and noise in a browser or a pasted support
 * log, so the console keeps them and the ring does not.
 */
static size_t strip_ansi(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    size_t o = 0;
    for (size_t i = 0; i < src_len && o < dst_size; i++) {
        if (src[i] == '\033') {
            /* Skip up to the final byte of the sequence (@ through ~). */
            i++;
            if (i < src_len && src[i] == '[') {
                i++;
                while (i < src_len && !(src[i] >= '@' && src[i] <= '~')) i++;
            }
            continue;                    /* loop's i++ steps past the final byte */
        }
        dst[o++] = src[i];
    }
    return o;
}

static int log_ring_vprintf(const char *fmt, va_list ap)
{
    char raw[LOG_LINE_MAX];
    char clean[LOG_LINE_MAX];

    /* vsnprintf consumes the va_list, and the original still has to be handed
     * to the console handler, so format from a copy. */
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(raw, sizeof(raw), fmt, ap2);
    va_end(ap2);

    if (n > 0) {
        size_t len = ((size_t)n < sizeof(raw)) ? (size_t)n : sizeof(raw) - 1;
        size_t clean_len = strip_ansi(clean, sizeof(clean), raw, len);
        ring_append(clean, clean_len);
    }

    return s_prev ? s_prev(fmt, ap) : 0;
}

void log_ring_init(void)
{
    if (s_prev) return;                 /* already hooked */
    s_prev = esp_log_set_vprintf(log_ring_vprintf);
}

size_t log_ring_snapshot(char *out, size_t out_size, int tail_lines)
{
    if (!out || out_size == 0) return 0;

    portENTER_CRITICAL(&s_mux);
    size_t head = s_head;
    bool wrapped = s_wrapped;
    portEXIT_CRITICAL(&s_mux);

    /* Copy oldest first. Reading outside the lock can tear against a
     * concurrent write; the cost is a garbled line in a diagnostic dump, which
     * is a fair trade for not blocking every logging task behind an HTTP
     * request. */
    size_t avail = wrapped ? LOG_RING_SIZE : head;
    if (avail > out_size - 1) avail = out_size - 1;

    size_t written = 0;
    if (wrapped) {
        size_t start = head;                       /* oldest byte */
        size_t tail_bytes = LOG_RING_SIZE - start;
        if (tail_bytes > avail) tail_bytes = avail;
        memcpy(out, s_ring + start, tail_bytes);
        written = tail_bytes;
        if (written < avail) {
            memcpy(out + written, s_ring, avail - written);
            written = avail;
        }
    } else {
        memcpy(out, s_ring, avail);
        written = avail;
    }
    out[written] = '\0';

    if (tail_lines > 0) {
        /* Walk back from the end counting newlines, then shift. */
        int seen = 0;
        size_t i = written;
        while (i > 0) {
            if (out[i - 1] == '\n' && ++seen > tail_lines) break;
            i--;
        }
        if (i > 0) {
            memmove(out, out + i, written - i);
            written -= i;
            out[written] = '\0';
        }
    }
    return written;
}

/* ── GET /api/logs ─────────────────────────────────────────────────── */

static esp_err_t handle_logs(httpd_req_t *req)
{
    int tail = 0;
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char n_val[12];
        if (httpd_query_key_value(query, "n", n_val, sizeof(n_val)) == ESP_OK)
            tail = atoi(n_val);
    }

    /* Heap, not stack: the httpd task cannot spare 8 KB. */
    char *buf = malloc(LOG_RING_SIZE + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }

    size_t n = log_ring_snapshot(buf, LOG_RING_SIZE + 1, tail);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t rc = httpd_resp_send(req, buf, n);
    free(buf);
    return rc;
}

esp_err_t log_ring_register(httpd_handle_t server)
{
    const httpd_uri_t uri = {
        .uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs, .user_ctx = NULL
    };
    return httpd_register_uri_handler(server, &uri);
}
