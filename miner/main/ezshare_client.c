/**
 * @file ezshare_client.c
 * @brief Miner HTTP client — streaming downloads with Range support.
 *
 * Ported from cpapdash-push-c3. No file buffering — chunks via callback.
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <regex.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ezshare_client.h"
#include "config.h"

static const char *TAG = LOG_TAG_EZSHARE;
static bool ezshare_initialized = false;

typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    http_response_t *response = (http_response_t *)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (response->size + evt->data_len > response->capacity) {
                size_t new_capacity = response->capacity * 2;
                if (new_capacity < response->size + evt->data_len)
                    new_capacity = response->size + evt->data_len + HTTP_BUFFER_SIZE;
                uint8_t *new_buffer = realloc(response->buffer, new_capacity);
                if (!new_buffer) return ESP_FAIL;
                response->buffer = new_buffer;
                response->capacity = new_capacity;
            }
            memcpy(response->buffer + response->size, evt->data, evt->data_len);
            response->size += evt->data_len;
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void url_encode_path(const char *src, char *dst, size_t dst_size) {
    char *d = dst;
    const char *end = dst + dst_size - 4;
    while (*src && d < end) {
        if (*src == '\\') { *d++ = '%'; *d++ = '5'; *d++ = 'C'; }
        else { *d++ = *src; }
        src++;
    }
    *d = '\0';
}

esp_err_t ezshare_client_init(void) {
    if (ezshare_initialized) return ESP_OK;
    ezshare_initialized = true;
    ESP_LOGI(TAG, "ezShare client initialized (%s:%d)", EZSHARE_IP, EZSHARE_PORT);
    return ESP_OK;
}

/* ── Directory listing (small responses, buffered) ─────────────── */

static esp_err_t parse_date_folders(const char *html, size_t html_len,
                                    char date_folders[][16], size_t max_folders,
                                    size_t *folder_count) {
    regex_t regex;
    regmatch_t matches[1];
    if (regcomp(&regex, "[0-9]{8}", REG_EXTENDED) != 0) return ESP_FAIL;

    *folder_count = 0;
    const char *cursor = html;
    size_t remaining = html_len;

    while (*folder_count < max_folders && remaining > 0) {
        if (regexec(&regex, cursor, 1, matches, 0) == 0) {
            int match_len = matches[0].rm_eo - matches[0].rm_so;
            if (match_len == 8) {
                strncpy(date_folders[*folder_count], cursor + matches[0].rm_so, 8);
                date_folders[*folder_count][8] = '\0';
                (*folder_count)++;
            }
            cursor += matches[0].rm_eo;
            remaining -= matches[0].rm_eo;
        } else break;
    }
    regfree(&regex);
    return ESP_OK;
}

static esp_err_t parse_file_listing(const char *html, size_t html_len,
                                    const char *date_folder,
                                    ezshare_file_list_t *file_list,
                                    time_t min_timestamp) {
    const char *cursor = html;
    const char *end = html + html_len;

    while (cursor < end) {
        const char *edf_ptr = strstr(cursor, ".edf");
        if (!edf_ptr) break;

        const char *filename_start = edf_ptr;
        while (filename_start > cursor && filename_start[-1] != '/' && filename_start[-1] != '\\')
            filename_start--;

        size_t filename_len = edf_ptr + 4 - filename_start;
        if (filename_len >= 64) { cursor = edf_ptr + 4; continue; }

        ezshare_file_t file = {0};
        strncpy(file.filename, filename_start, filename_len);
        file.filename[filename_len] = '\0';
        snprintf(file.path, sizeof(file.path), "DATALOG\\%s\\%s", date_folder, file.filename);

        ezshare_file_list_add(file_list, &file);
        cursor = edf_ptr + 4;
    }
    return ESP_OK;
}

esp_err_t ezshare_list_date_folders(char date_folders[][16], size_t max_folders,
                                     size_t *folder_count) {
    if (!ezshare_initialized) return ESP_ERR_INVALID_STATE;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d%s", EZSHARE_IP, EZSHARE_PORT, EZSHARE_DIR_PATH);

    http_response_t response = { .buffer = malloc(HTTP_BUFFER_SIZE), .size = 0, .capacity = HTTP_BUFFER_SIZE };
    if (!response.buffer) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = { .url = url, .event_handler = http_event_handler,
                                         .user_data = &response, .timeout_ms = HTTP_TIMEOUT_MS };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(response.buffer); return ESP_FAIL; }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200)
        err = parse_date_folders((const char *)response.buffer, response.size,
                                date_folders, max_folders, folder_count);
    else if (err == ESP_OK) err = ESP_FAIL;

    esp_http_client_cleanup(client);
    free(response.buffer);
    return err;
}

esp_err_t ezshare_list_files(const char *date_folder, ezshare_file_list_t *file_list,
                              time_t min_timestamp) {
    if (!ezshare_initialized || !date_folder || !file_list) return ESP_ERR_INVALID_ARG;

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/dir?dir=A:DATALOG%%5C%s",
             EZSHARE_IP, EZSHARE_PORT, date_folder);

    http_response_t response = { .buffer = malloc(HTTP_BUFFER_SIZE), .size = 0, .capacity = HTTP_BUFFER_SIZE };
    if (!response.buffer) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = { .url = url, .event_handler = http_event_handler,
                                         .user_data = &response, .timeout_ms = HTTP_TIMEOUT_MS };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(response.buffer); return ESP_FAIL; }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200)
        err = parse_file_listing((const char *)response.buffer, response.size,
                                date_folder, file_list, min_timestamp);
    else if (err == ESP_OK) err = ESP_FAIL;

    esp_http_client_cleanup(client);
    free(response.buffer);
    return err;
}

/* ── Streaming downloads with Range support ────────────────────── */

esp_err_t ezshare_raw_get_range(const char *path, size_t chunk_size,
                                 uint32_t range_start, uint32_t range_end,
                                 uint16_t *out_http_status,
                                 uint32_t *out_content_length,
                                 raw_chunk_callback_t callback, void *ctx) {
    if (!ezshare_initialized || !path || !callback || chunk_size == 0)
        return ESP_ERR_INVALID_ARG;

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d%s", EZSHARE_IP, EZSHARE_PORT, path);

    uint8_t *chunk_buf = malloc(chunk_size);
    if (!chunk_buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = { .url = url, .timeout_ms = HTTP_TIMEOUT_MS,
                                         .buffer_size = chunk_size };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(chunk_buf); return ESP_ERR_NO_MEM; }

    bool has_range = (range_start > 0 || range_end > 0);
    if (has_range) {
        char range_hdr[64];
        if (range_end > 0)
            snprintf(range_hdr, sizeof(range_hdr), "bytes=%lu-%lu",
                     (unsigned long)range_start, (unsigned long)range_end);
        else
            snprintf(range_hdr, sizeof(range_hdr), "bytes=%lu-", (unsigned long)range_start);
        esp_http_client_set_header(client, "Range", range_hdr);
        ESP_LOGI(TAG, "Range: %s", range_hdr);
    }

    esp_err_t result = ESP_OK;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client); free(chunk_buf); return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200 && status_code != 206) {
        ESP_LOGE(TAG, "HTTP %d (url=%s)", status_code, url);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        free(chunk_buf); return ESP_ERR_HTTP_BASE + status_code;
    }

    if (out_http_status) *out_http_status = (uint16_t)status_code;
    if (out_content_length) *out_content_length = (content_length > 0) ? (uint32_t)content_length : 0;

    ESP_LOGI(TAG, "HTTP %d, cl=%d (url=%s)", status_code, content_length, url);

    size_t seq = 0;
    bool have_peek = false;
    uint8_t peek_byte = 0;

    while (true) {
        size_t filled = 0;
        if (have_peek) { chunk_buf[0] = peek_byte; filled = 1; have_peek = false; }

        while (filled < chunk_size) {
            int n = esp_http_client_read(client, (char *)chunk_buf + filled, chunk_size - filled);
            if (n < 0) { result = ESP_FAIL; break; }
            if (n == 0) break;
            filled += (size_t)n;
        }
        if (result != ESP_OK) break;
        if (filled == 0) break;

        bool is_last = (filled < chunk_size);
        if (!is_last) {
            int n = esp_http_client_read(client, (char *)&peek_byte, 1);
            if (n < 0) { result = ESP_FAIL; break; }
            if (n == 0) is_last = true;
            else have_peek = true;
        }

        err = callback(chunk_buf, filled, seq, is_last, ctx);
        if (err != ESP_OK) { result = err; break; }
        seq++;
        if (is_last) break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(chunk_buf);
    return result;
}

esp_err_t ezshare_raw_get(const char *path, size_t chunk_size,
                           raw_chunk_callback_t callback, void *ctx) {
    return ezshare_raw_get_range(path, chunk_size, 0, 0, NULL, NULL, callback, ctx);
}

/* ── File list helpers ─────────────────────────────────────────── */

esp_err_t ezshare_file_list_init(ezshare_file_list_t *file_list, size_t initial_capacity) {
    if (!file_list) return ESP_ERR_INVALID_ARG;
    file_list->files = calloc(initial_capacity, sizeof(ezshare_file_t));
    if (!file_list->files) return ESP_ERR_NO_MEM;
    file_list->count = 0;
    file_list->capacity = initial_capacity;
    return ESP_OK;
}

esp_err_t ezshare_file_list_add(ezshare_file_list_t *file_list, const ezshare_file_t *file) {
    if (!file_list || !file) return ESP_ERR_INVALID_ARG;
    if (file_list->count >= file_list->capacity) {
        size_t new_cap = file_list->capacity * 2;
        ezshare_file_t *new_files = realloc(file_list->files, new_cap * sizeof(ezshare_file_t));
        if (!new_files) return ESP_ERR_NO_MEM;
        file_list->files = new_files;
        file_list->capacity = new_cap;
    }
    file_list->files[file_list->count++] = *file;
    return ESP_OK;
}

void ezshare_file_list_free(ezshare_file_list_t *file_list) {
    if (!file_list) return;
    free(file_list->files);
    file_list->files = NULL;
    file_list->count = 0;
    file_list->capacity = 0;
}

void ezshare_client_deinit(void) {
    ezshare_initialized = false;
}
