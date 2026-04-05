/**
 * @file ezshare_client.c
 * @brief Scanner C3 ez Share HTTP Client
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <regex.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "ezshare_client.h"
#include "config.h"

static const char *TAG = LOG_TAG_EZSHARE;
static bool ezshare_initialized = false;

// HTTP response buffer
typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
} http_response_t;

/**
 * @brief HTTP event handler callback
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    http_response_t *response = (http_response_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Expand buffer if needed
            if (response->size + evt->data_len > response->capacity) {
                size_t new_capacity = response->capacity * 2;
                if (new_capacity < response->size + evt->data_len) {
                    new_capacity = response->size + evt->data_len + HTTP_BUFFER_SIZE;
                }

                uint8_t *new_buffer = realloc(response->buffer, new_capacity);
                if (new_buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to expand HTTP buffer");
                    return ESP_FAIL;
                }

                response->buffer = new_buffer;
                response->capacity = new_capacity;
            }

            // Append data to buffer
            memcpy(response->buffer + response->size, evt->data, evt->data_len);
            response->size += evt->data_len;
            break;

        default:
            break;
    }

    return ESP_OK;
}

/**
 * @brief Initialize ez Share client
 */
esp_err_t ezshare_client_init(void) {
    if (ezshare_initialized) {
        ESP_LOGW(TAG, "ez Share client already initialized");
        return ESP_OK;
    }

    ezshare_initialized = true;
    ESP_LOGI(TAG, "ez Share client initialized (host: %s:%d)", EZSHARE_IP, EZSHARE_PORT);

    return ESP_OK;
}

/**
 * @brief Parse HTML directory listing to extract date folders
 */
static esp_err_t parse_date_folders(const char *html, size_t html_len,
                                    char date_folders[][16], size_t max_folders,
                                    size_t *folder_count) {
    // Regex pattern for date folders: YYYYMMDD
    const char *pattern = "[0-9]{8}";
    regex_t regex;
    regmatch_t matches[1];

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        ESP_LOGE(TAG, "Failed to compile regex");
        return ESP_FAIL;
    }

    *folder_count = 0;
    const char *cursor = html;
    size_t remaining = html_len;

    while (*folder_count < max_folders && remaining > 0) {
        if (regexec(&regex, cursor, 1, matches, 0) == 0) {
            // Extract match
            int match_len = matches[0].rm_eo - matches[0].rm_so;
            if (match_len == 8) {  // YYYYMMDD format
                strncpy(date_folders[*folder_count], cursor + matches[0].rm_so, 8);
                date_folders[*folder_count][8] = '\0';
                (*folder_count)++;
            }

            // Move cursor past this match
            cursor += matches[0].rm_eo;
            remaining -= matches[0].rm_eo;
        } else {
            break;
        }
    }

    regfree(&regex);
    ESP_LOGI(TAG, "Found %d date folders", *folder_count);

    return ESP_OK;
}

/**
 * @brief Parse HTML directory listing to extract files
 */
static esp_err_t parse_file_listing(const char *html, size_t html_len,
                                    const char *date_folder,
                                    ezshare_file_list_t *file_list,
                                    time_t min_timestamp) {
    // Simple parsing: look for .edf files in HTML
    // Example: <a href="/download?file=DATALOG%5C20260204%5C20260204_001809_BRP.edf">
    const char *cursor = html;
    const char *end = html + html_len;

    while (cursor < end) {
        // Find .edf extension
        const char *edf_ptr = strstr(cursor, ".edf");
        if (edf_ptr == NULL) {
            break;
        }

        // Backtrack to find filename start
        const char *filename_start = edf_ptr;
        while (filename_start > cursor && filename_start[-1] != '/' && filename_start[-1] != '\\') {
            filename_start--;
        }

        // Extract filename
        size_t filename_len = edf_ptr + 4 - filename_start;
        if (filename_len >= sizeof(((ezshare_file_t *)0)->filename)) {
            cursor = edf_ptr + 4;
            continue;
        }

        ezshare_file_t file = {0};
        strncpy(file.filename, filename_start, filename_len);
        file.filename[filename_len] = '\0';

        // Build full path
        snprintf(file.path, sizeof(file.path), "DATALOG\\%s\\%s", date_folder, file.filename);

        // TODO: Parse file size and timestamp from HTML if available
        file.size = 0;
        file.timestamp = 0;
        file.content = NULL;

        // Add to file list
        ezshare_file_list_add(file_list, &file);

        cursor = edf_ptr + 4;
    }

    ESP_LOGI(TAG, "Parsed %d files from date folder %s", file_list->count, date_folder);

    return ESP_OK;
}

/**
 * @brief List date folders in DATALOG directory
 */
esp_err_t ezshare_list_date_folders(char date_folders[][16], size_t max_folders, size_t *folder_count) {
    if (!ezshare_initialized) {
        ESP_LOGE(TAG, "ez Share client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Build URL
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d%s", EZSHARE_IP, EZSHARE_PORT, EZSHARE_DIR_PATH);

    ESP_LOGI(TAG, "Listing date folders: %s", url);

    // Initialize HTTP response buffer
    http_response_t response = {
        .buffer = malloc(HTTP_BUFFER_SIZE),
        .size = 0,
        .capacity = HTTP_BUFFER_SIZE
    };

    if (response.buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
        return ESP_ERR_NO_MEM;
    }

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(response.buffer);
        return ESP_FAIL;
    }

    // Perform HTTP GET
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                 status_code, response.size);

        if (status_code == 200) {
            // Parse HTML to extract date folders
            err = parse_date_folders((const char *)response.buffer, response.size,
                                    date_folders, max_folders, folder_count);
        } else {
            ESP_LOGE(TAG, "HTTP GET failed with status %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(response.buffer);

    return err;
}

/**
 * @brief List files in a specific date folder
 */
esp_err_t ezshare_list_files(const char *date_folder, ezshare_file_list_t *file_list, time_t min_timestamp) {
    if (!ezshare_initialized) {
        ESP_LOGE(TAG, "ez Share client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (date_folder == NULL || file_list == NULL) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    // Build URL with URL-encoded path
    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/dir?dir=A:DATALOG%%5C%s",
             EZSHARE_IP, EZSHARE_PORT, date_folder);

    ESP_LOGI(TAG, "Listing files in folder: %s", date_folder);

    // Initialize HTTP response buffer
    http_response_t response = {
        .buffer = malloc(HTTP_BUFFER_SIZE),
        .size = 0,
        .capacity = HTTP_BUFFER_SIZE
    };

    if (response.buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
        return ESP_ERR_NO_MEM;
    }

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(response.buffer);
        return ESP_FAIL;
    }

    // Perform HTTP GET
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                 status_code, response.size);

        if (status_code == 200) {
            // Parse HTML to extract file list
            err = parse_file_listing((const char *)response.buffer, response.size,
                                    date_folder, file_list, min_timestamp);
        } else {
            ESP_LOGE(TAG, "HTTP GET failed with status %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(response.buffer);

    return err;
}

/**
 * @brief Download a single file from ez Share
 */
esp_err_t ezshare_download_file(const char *file_path, uint8_t **content_out, size_t *size_out) {
    if (!ezshare_initialized) {
        ESP_LOGE(TAG, "ez Share client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (file_path == NULL || content_out == NULL || size_out == NULL) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    // Build URL with URL-encoded path (replace \ with %5C)
    char url[512];
    char encoded_path[256];
    const char *src = file_path;
    char *dst = encoded_path;

    while (*src && (dst - encoded_path) < sizeof(encoded_path) - 4) {
        if (*src == '\\') {
            *dst++ = '%';
            *dst++ = '5';
            *dst++ = 'C';
        } else {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';

    snprintf(url, sizeof(url), "http://%s:%d/download?file=%s",
             EZSHARE_IP, EZSHARE_PORT, encoded_path);

    ESP_LOGI(TAG, "Downloading file: %s", file_path);

    // Initialize HTTP response buffer
    http_response_t response = {
        .buffer = malloc(HTTP_BUFFER_SIZE),
        .size = 0,
        .capacity = HTTP_BUFFER_SIZE
    };

    if (response.buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
        return ESP_ERR_NO_MEM;
    }

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(response.buffer);
        return ESP_FAIL;
    }

    // Perform HTTP GET
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                 status_code, response.size);

        if (status_code == 200) {
            *content_out = response.buffer;
            *size_out = response.size;
            ESP_LOGI(TAG, "Downloaded file: %s (%d bytes)", file_path, response.size);
        } else {
            ESP_LOGE(TAG, "HTTP GET failed with status %d", status_code);
            free(response.buffer);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
        free(response.buffer);
    }

    esp_http_client_cleanup(client);

    return err;
}

/**
 * @brief Initialize file list
 */
esp_err_t ezshare_file_list_init(ezshare_file_list_t *file_list, size_t initial_capacity) {
    if (file_list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    file_list->files = calloc(initial_capacity, sizeof(ezshare_file_t));
    if (file_list->files == NULL) {
        ESP_LOGE(TAG, "Failed to allocate file list");
        return ESP_ERR_NO_MEM;
    }

    file_list->count = 0;
    file_list->capacity = initial_capacity;

    return ESP_OK;
}

/**
 * @brief Add file to file list
 */
esp_err_t ezshare_file_list_add(ezshare_file_list_t *file_list, const ezshare_file_t *file) {
    if (file_list == NULL || file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Expand list if needed
    if (file_list->count >= file_list->capacity) {
        size_t new_capacity = file_list->capacity * 2;
        ezshare_file_t *new_files = realloc(file_list->files, new_capacity * sizeof(ezshare_file_t));
        if (new_files == NULL) {
            ESP_LOGE(TAG, "Failed to expand file list");
            return ESP_ERR_NO_MEM;
        }

        file_list->files = new_files;
        file_list->capacity = new_capacity;
    }

    // Copy file metadata (but not content)
    file_list->files[file_list->count] = *file;
    file_list->count++;

    return ESP_OK;
}

/**
 * @brief Free file list and all file contents
 */
void ezshare_file_list_free(ezshare_file_list_t *file_list) {
    if (file_list == NULL) {
        return;
    }

    if (file_list->files != NULL) {
        // Free all file contents
        for (size_t i = 0; i < file_list->count; i++) {
            if (file_list->files[i].content != NULL) {
                free(file_list->files[i].content);
            }
        }

        free(file_list->files);
        file_list->files = NULL;
    }

    file_list->count = 0;
    file_list->capacity = 0;
}

/**
 * @brief Deinitialize ez Share client
 */
void ezshare_client_deinit(void) {
    if (ezshare_initialized) {
        ezshare_initialized = false;
        ESP_LOGI(TAG, "ez Share client deinitialized");
    }
}
