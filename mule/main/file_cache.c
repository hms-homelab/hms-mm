#include "file_cache.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cache";

static cached_file_t s_files[FILE_CACHE_MAX_FILES];
static size_t s_count = 0;
static size_t s_total_bytes = 0;

void file_cache_init(void)
{
    memset(s_files, 0, sizeof(s_files));
    s_count = 0;
    s_total_bytes = 0;
}

bool file_cache_put(const char *path, uint8_t *content, size_t size)
{
    // Check if file already exists (update in place)
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_files[i].path, path) == 0) {
            s_total_bytes -= s_files[i].size;
            free(s_files[i].content);
            s_files[i].content = content;
            s_files[i].size = size;
            s_total_bytes += size;
            ESP_LOGI(TAG, "Updated: %s (%zu bytes)", path, size);
            return true;
        }
    }

    if (s_count >= FILE_CACHE_MAX_FILES) {
        ESP_LOGW(TAG, "Cache full (%d files), dropping oldest", FILE_CACHE_MAX_FILES);
        // Drop oldest
        free(s_files[0].content);
        s_total_bytes -= s_files[0].size;
        memmove(&s_files[0], &s_files[1], (s_count - 1) * sizeof(cached_file_t));
        s_count--;
    }

    strncpy(s_files[s_count].path, path, sizeof(s_files[0].path) - 1);
    s_files[s_count].content = content;
    s_files[s_count].size = size;
    s_count++;
    s_total_bytes += size;

    ESP_LOGI(TAG, "Cached: %s (%zu bytes, total: %zu/%d)",
             path, size, s_count, FILE_CACHE_MAX_FILES);
    return true;
}

const cached_file_t *file_cache_get(const char *path)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_files[i].path, path) == 0) return &s_files[i];
    }
    return NULL;
}

const cached_file_t *file_cache_get_all(size_t *count)
{
    *count = s_count;
    return s_files;
}

void file_cache_clear(void)
{
    for (size_t i = 0; i < s_count; i++) {
        free(s_files[i].content);
    }
    memset(s_files, 0, sizeof(s_files));
    s_count = 0;
    s_total_bytes = 0;
}

size_t file_cache_count(void) { return s_count; }
size_t file_cache_total_bytes(void) { return s_total_bytes; }
