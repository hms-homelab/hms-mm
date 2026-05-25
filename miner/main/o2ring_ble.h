/**
 * @file o2ring_ble.h
 * @brief Wellue O2 Ring BLE GATT client
 *
 * Two modes of operation:
 * - Persistent connection: scan/connect once, stay connected for file ops
 * - Poll cycle: connect → read sensors → disconnect → sleep (for live data)
 */

#ifndef O2RING_BLE_H
#define O2RING_BLE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define O2RING_MAX_FILES     4
#define O2RING_MAX_FILENAME  32

typedef struct {
    char name[O2RING_MAX_FILENAME];
} o2ring_file_entry_t;

typedef struct {
    char     model[32];
    char     serial[32];
    uint8_t  battery;
    o2ring_file_entry_t files[O2RING_MAX_FILES];
    int      file_count;
    bool     valid;
} o2ring_device_info_t;

typedef struct {
    uint8_t  spo2;      /* 0-100, 0xFF = invalid */
    uint8_t  hr;        /* bpm, 0xFF = invalid */
    uint8_t  motion;    /* 0-255 accelerometer */
    uint8_t  vibration; /* 0/1 haptic alert */
    int64_t  timestamp; /* esp_timer_get_time() when sampled */
    bool     valid;
} o2ring_live_t;

esp_err_t o2ring_ble_init(void);
void o2ring_ble_deinit(void);
esp_err_t o2ring_ble_start_scan(void);
esp_err_t o2ring_ble_stop(void);
esp_err_t o2ring_ble_disconnect(void);
bool o2ring_ble_is_connected(void);

/** Control auto-reconnect after BLE disconnect. Default: true. */
void o2ring_ble_set_auto_reconnect(bool enable);

/**
 * Scan, connect, and block until ready (notifications enabled).
 * @param timeout_ms  Max time to wait
 * @return ESP_OK if ready, ESP_ERR_TIMEOUT if not connected in time
 */
esp_err_t o2ring_ble_connect_and_wait(uint32_t timeout_ms);

esp_err_t o2ring_ble_refresh_info(void);
const o2ring_device_info_t *o2ring_ble_get_info(void);

/**
 * Send READ_SENSORS (0x17), block until response. Caches live data.
 * @return ESP_OK on success
 */
esp_err_t o2ring_ble_read_sensors(void);

/** Get cached live reading (from last read_sensors call). */
const o2ring_live_t *o2ring_ble_get_live(void);

esp_err_t o2ring_ble_download_file(const char *filename,
                                    uint8_t *out_buf, size_t buf_size,
                                    size_t *out_len);

/** Callback for streaming download. Return true to continue, false to abort. */
typedef bool (*o2ring_chunk_cb_t)(const uint8_t *data, size_t len,
                                   uint32_t offset, void *ctx);

/**
 * Download a .vld file, streaming each BLE block through a callback.
 * No buffer needed — each block fires the callback immediately.
 */
esp_err_t o2ring_ble_download_file_stream(const char *filename,
                                           o2ring_chunk_cb_t cb, void *ctx,
                                           size_t *out_len);

#endif /* O2RING_BLE_H */
