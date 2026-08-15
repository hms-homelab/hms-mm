#pragma once

/**
 * @file ota_miner.h
 * @brief Streaming a firmware image to the miner over the UART link.
 *
 * The miner has no network, so the mule is its only route to a new image. Both
 * sources end up in the same place: chunks of base64 with a CRC, acknowledged
 * one at a time, then a finish that makes the miner validate and restart.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Open a transfer. Holds the link until ota_miner_end().
 * @param total_size exact image size; the miner refuses a short image.
 */
esp_err_t ota_miner_begin(uint32_t total_size);

/** Send the next slice, in order. Blocks for the miner's acknowledgement. */
esp_err_t ota_miner_write(const uint8_t *data, size_t len);

/**
 * @brief Finish or abandon the transfer, and release the link.
 * @param commit true to have the miner validate and restart, false to abort.
 */
esp_err_t ota_miner_end(bool commit);

/** Bytes accepted so far, for progress reporting. */
uint32_t ota_miner_written(void);
