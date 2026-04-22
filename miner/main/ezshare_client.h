/**
 * @file ezshare_client.h
 * @brief Miner HTTP client — streaming downloads from ezShare/Fysetc
 *
 * No file buffering: chunks delivered via callback, one at a time.
 */

#ifndef EZSHARE_CLIENT_H
#define EZSHARE_CLIENT_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

typedef struct {
    char path[256];
    char filename[64];
    size_t size;
    time_t timestamp;
} ezshare_file_t;

typedef struct {
    ezshare_file_t *files;
    size_t count;
    size_t capacity;
} ezshare_file_list_t;

typedef esp_err_t (*raw_chunk_callback_t)(const uint8_t *data, size_t len,
                                          size_t seq, bool is_last, void *ctx);

esp_err_t ezshare_client_init(void);
void ezshare_client_deinit(void);

esp_err_t ezshare_list_date_folders(char date_folders[][16], size_t max_folders,
                                     size_t *folder_count);

esp_err_t ezshare_list_files(const char *date_folder, ezshare_file_list_t *file_list,
                              time_t min_timestamp);

/**
 * @brief HTTP GET with optional Range header, streamed via callback.
 *
 * @param path             URL path + query (e.g. "/download?file=...")
 * @param chunk_size       Bytes per chunk (e.g. 4096)
 * @param range_start      First byte offset (0 = no range)
 * @param range_end        Last byte offset (0 = open-ended)
 * @param out_http_status  Receives HTTP status (200/206), may be NULL
 * @param out_content_length Receives content-length, may be NULL
 * @param callback         Called for each chunk
 * @param ctx              User context
 */
esp_err_t ezshare_raw_get_range(const char *path, size_t chunk_size,
                                 uint32_t range_start, uint32_t range_end,
                                 uint16_t *out_http_status,
                                 uint32_t *out_content_length,
                                 raw_chunk_callback_t callback, void *ctx);

esp_err_t ezshare_raw_get(const char *path, size_t chunk_size,
                           raw_chunk_callback_t callback, void *ctx);

esp_err_t ezshare_file_list_init(ezshare_file_list_t *file_list, size_t initial_capacity);
esp_err_t ezshare_file_list_add(ezshare_file_list_t *file_list, const ezshare_file_t *file);
void ezshare_file_list_free(ezshare_file_list_t *file_list);

#endif // EZSHARE_CLIENT_H
