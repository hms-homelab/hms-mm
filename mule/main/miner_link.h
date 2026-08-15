#pragma once

/**
 * @file miner_link.h
 * @brief Mule-side control exchanges with the miner over the UART link.
 *
 * The bulk data path (/dir, /download, /o2ring) lives in file_server.c. This
 * module owns the small request/response conversations that are not about
 * moving a file: firmware version, reboot, reset, and the O2Ring kill switch —
 * plus the ezShare link diagnostics the miner attaches to its errors.
 *
 * Every call here takes the shared link lock (uart_link_lock) for the whole
 * exchange, so these can be issued from an HTTP handler while a proxy transfer
 * is in flight without the two stealing each other's replies.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Snapshot of the miner's view of the ezShare link, from its last error. */
typedef struct {
    bool     valid;      /**< false until the miner has reported at least once */
    bool     assoc;      /**< associated with the ezShare AP */
    int      rssi;       /**< dBm, 0 when not associated */
    int      reason;     /**< last raw WIFI_REASON_* (201 = AP not found, 202 = bad password) */
    int64_t  when_us;    /**< esp_timer_get_time() when recorded */
} miner_ezshare_diag_t;

/**
 * @brief Ask the miner for its firmware version.
 *
 * Result is cached, so /api/status can report it without a wire round-trip on
 * every poll. Returns false if the miner does not answer.
 */
bool miner_link_query_version(char *out, size_t out_size);

/**
 * @brief Last known miner version, or "unknown" if never successfully read.
 *
 * Never blocks and never touches the link.
 */
const char *miner_link_cached_version(void);

/** Reboot the miner. Config is left intact. */
bool miner_link_reboot(void);

/** Erase the miner's config and reboot it. The mule re-pushes ezShare
 *  credentials with set_config on its next boot. */
bool miner_link_reset(void);

/** Read the O2Ring BLE gate (miner NVS ble_active). */
bool miner_link_get_o2_enabled(bool *enabled);

/**
 * @brief Set the O2Ring BLE gate.
 *
 * The miner restarts to apply it, because the BLE stack is only brought up (or
 * not) at init. A no-op change does not restart.
 */
bool miner_link_set_o2_enabled(bool enabled);

/** Record diagnostics seen on a miner error frame. Called by file_server. */
void miner_link_note_diag(bool assoc, int rssi, int reason);

/** Latest diagnostics snapshot. */
void miner_link_get_diag(miner_ezshare_diag_t *out);
