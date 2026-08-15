/**
 * @file ota_handler.c
 * @brief Miner-side OTA over the link (see ota_handler.h).
 */

#include "ota_handler.h"
#include "config.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "miner_ota";

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static bool     s_active;
static uint32_t s_total;
static uint32_t s_written;

/* Rollback watchdog state. */
static bool    s_pending;        /* running an image awaiting confirmation */
static bool    s_confirmed;
static int64_t s_deadline_us;

/* ── writing an image ─────────────────────────────────────────────── */

esp_err_t miner_ota_begin(uint32_t total_size)
{
    if (s_active) {
        ESP_LOGW(TAG, "begin while a transfer is already open — abandoning the old one");
        miner_ota_abort();
    }

    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        ESP_LOGE(TAG, "no OTA partition available");
        return ESP_ERR_NOT_FOUND;
    }
    if (total_size > s_part->size) {
        ESP_LOGE(TAG, "image is %lu B, partition holds %lu B",
                 (unsigned long)total_size, (unsigned long)s_part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Pass the real size rather than OTA_SIZE_UNKNOWN: it lets the driver
     * erase exactly what is needed instead of the whole partition, which on
     * this chip is the difference between a couple of seconds and much longer
     * spent with the link unattended. */
    esp_err_t err = esp_ota_begin(s_part, total_size, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    s_active  = true;
    s_total   = total_size;
    s_written = 0;
    ESP_LOGW(TAG, "OTA started: %lu B into %s", (unsigned long)total_size, s_part->label);
    return ESP_OK;
}

esp_err_t miner_ota_write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (!s_active) return ESP_ERR_INVALID_STATE;

    /* esp_ota_write appends; it has no notion of offset. Checking the mule's
     * offset against our own count is what turns a silently misordered or
     * dropped chunk into a clean failure rather than a corrupt image that only
     * reveals itself as a boot loop. */
    if (offset != s_written) {
        ESP_LOGE(TAG, "chunk offset %lu, expected %lu — aborting",
                 (unsigned long)offset, (unsigned long)s_written);
        miner_ota_abort();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_written + len > s_total) {
        ESP_LOGE(TAG, "image longer than declared — aborting");
        miner_ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        miner_ota_abort();
        return err;
    }
    s_written += len;

    /* One line per 64 KB: enough to watch progress on a console without the
     * log itself becoming the bottleneck. */
    if ((s_written % (64 * 1024)) < len)
        ESP_LOGI(TAG, "OTA %lu/%lu KB",
                 (unsigned long)(s_written / 1024), (unsigned long)(s_total / 1024));
    return ESP_OK;
}

esp_err_t miner_ota_finish(void)
{
    if (!s_active) return ESP_ERR_INVALID_STATE;

    if (s_written != s_total) {
        ESP_LOGE(TAG, "short image: %lu of %lu B — refusing",
                 (unsigned long)s_written, (unsigned long)s_total);
        miner_ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    /* Validates the image, including its appended SHA256. */
    esp_err_t err = esp_ota_end(s_handle);
    s_active = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "OTA complete, booting %s on restart", s_part->label);
    return ESP_OK;
}

void miner_ota_abort(void)
{
    if (!s_active) return;
    esp_ota_abort(s_handle);
    s_active = false;
    s_written = 0;
    ESP_LOGW(TAG, "OTA aborted");
}

bool miner_ota_in_progress(void) { return s_active; }

/* ── rollback watchdog ────────────────────────────────────────────── */

void miner_ota_arm_rollback_watchdog(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    s_pending     = true;
    s_confirmed   = false;
    s_deadline_us = esp_timer_get_time() + (int64_t)MINER_OTA_CONFIRM_TIMEOUT_MS * 1000;
    ESP_LOGW(TAG, "running an unconfirmed image; %d s to prove the link works",
             MINER_OTA_CONFIRM_TIMEOUT_MS / 1000);
}

void miner_ota_confirm_boot(void)
{
    if (!s_pending || s_confirmed) return;
    s_confirmed = true;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK)
        ESP_LOGW(TAG, "link confirmed, image marked valid");
    else
        ESP_LOGE(TAG, "mark_app_valid: %s", esp_err_to_name(err));
}

void miner_ota_check_rollback_deadline(void)
{
    if (!s_pending || s_confirmed) return;
    if (esp_timer_get_time() < s_deadline_us) return;

    /* Deliberately restart WITHOUT marking the image valid: that is what makes
     * the bootloader fall back to the previous slot. The alternative is a
     * miner that boots happily and can never be reached again. */
    ESP_LOGE(TAG, "no frame from the mule within the deadline — rolling back");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}
