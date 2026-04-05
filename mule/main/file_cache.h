#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// In-memory cache of files received from miner via UART.
// Served to HTTP clients by file_server.

typedef struct {
    char path[256];         // e.g. "DATALOG/20260204/20260204_001809_BRP.edf"
    uint8_t *content;       // Raw file bytes (decoded from base64)
    size_t size;
} cached_file_t;

void file_cache_init(void);

// Add or update a file in the cache (takes ownership of content pointer).
bool file_cache_put(const char *path, uint8_t *content, size_t size);

// Get a cached file by path (returns NULL if not found).
const cached_file_t *file_cache_get(const char *path);

// Get all cached files and count.
const cached_file_t *file_cache_get_all(size_t *count);

// Clear all cached files (frees memory).
void file_cache_clear(void);

size_t file_cache_count(void);
size_t file_cache_total_bytes(void);
