#pragma once

/**
 * @file ota_handler.h
 * @brief Applying a firmware image handed over the UART link, and the
 *        rollback watchdog that makes doing so survivable.
 *
 * The miner has no network of its own and no console a user will ever reach.
 * The link to the mule is the only way in, so an image that boots but cannot
 * talk over that link is unrecoverable without opening the case. Hence the
 * watchdog: a freshly written image must prove it can still decode a frame
 * from the mule, within a deadline, or it reboots without confirming and the
 * bootloader reverts to the slot that was working.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Begin writing a new image. @param total_size bytes the mule will send. */
esp_err_t miner_ota_begin(uint32_t total_size);

/**
 * @brief Write one chunk.
 * @param offset  byte offset the mule believes this chunk starts at; used to
 *                catch a dropped or duplicated chunk rather than trusting the
 *                stream to have arrived in order.
 */
esp_err_t miner_ota_write(uint32_t offset, const uint8_t *data, size_t len);

/** Validate and activate the image. Caller reboots on success. */
esp_err_t miner_ota_finish(void);

/** Abandon a transfer in progress (link lost, bad chunk, mule gave up). */
void miner_ota_abort(void);

/** True while a transfer is open, so the dispatcher can refuse other work. */
bool miner_ota_in_progress(void);

/**
 * @brief Arm the rollback watchdog if this boot is running an unconfirmed image.
 *
 * Call once at startup. A no-op unless the running slot is PENDING_VERIFY.
 */
void miner_ota_arm_rollback_watchdog(void);

/**
 * @brief Report that the link is alive, confirming a pending image.
 *
 * Call on every successfully decoded frame from the mule. A decoded frame is
 * proof the link works; noise on the wire must never count.
 */
void miner_ota_confirm_boot(void);

/**
 * @brief Reboot if a pending image has run out of time to prove itself.
 *
 * Call periodically from the idle loop.
 */
void miner_ota_check_rollback_deadline(void);
