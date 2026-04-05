/**
 * @file ezshare_client.h
 * @brief Scanner C3 ez Share HTTP Client
 */

#ifndef EZSHARE_CLIENT_H
#define EZSHARE_CLIENT_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/**
 * @brief File metadata structure
 */
typedef struct {
    char path[256];          // Full file path (e.g., "DATALOG/20260204/20260204_001809_BRP.edf")
    char filename[64];       // Filename only
    size_t size;             // File size in bytes
    time_t timestamp;        // File modification time (Unix timestamp)
    uint8_t *content;        // File content (dynamically allocated)
} ezshare_file_t;

/**
 * @brief File list structure
 */
typedef struct {
    ezshare_file_t *files;   // Array of files
    size_t count;            // Number of files
    size_t capacity;         // Allocated capacity
} ezshare_file_list_t;

/**
 * @brief Initialize ez Share client
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ezshare_client_init(void);

/**
 * @brief List date folders in DATALOG directory
 * @param date_folders Output array of date folder names (e.g., "20260204")
 * @param max_folders Maximum number of folders to retrieve
 * @param folder_count Output parameter for actual number of folders found
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ezshare_list_date_folders(char date_folders[][16], size_t max_folders, size_t *folder_count);

/**
 * @brief List files in a specific date folder
 * @param date_folder Date folder name (e.g., "20260204")
 * @param file_list Output file list (must be initialized)
 * @param min_timestamp Only include files newer than this timestamp (0 = all files)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ezshare_list_files(const char *date_folder, ezshare_file_list_t *file_list, time_t min_timestamp);

/**
 * @brief Download a single file from ez Share
 * @param file_path File path relative to ez Share root (e.g., "DATALOG/20260204/file.edf")
 * @param content_out Output buffer for file content (dynamically allocated, caller must free)
 * @param size_out Output parameter for file size
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ezshare_download_file(const char *file_path, uint8_t **content_out, size_t *size_out);

/**
 * @brief Initialize file list
 * @param file_list File list to initialize
 * @param initial_capacity Initial capacity
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ezshare_file_list_init(ezshare_file_list_t *file_list, size_t initial_capacity);

/**
 * @brief Add file to file list
 * @param file_list File list
 * @param file File to add (content is copied)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ezshare_file_list_add(ezshare_file_list_t *file_list, const ezshare_file_t *file);

/**
 * @brief Free file list and all file contents
 * @param file_list File list to free
 */
void ezshare_file_list_free(ezshare_file_list_t *file_list);

/**
 * @brief Deinitialize ez Share client
 */
void ezshare_client_deinit(void);

#endif // EZSHARE_CLIENT_H
