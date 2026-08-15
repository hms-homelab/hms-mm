/**
 * @file crash_guard.c
 * @brief Crash-loop self-heal (see crash_guard.h).
 */

#include "crash_guard.h"
#include "config.h"
#include "nvs_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"

static const char *TAG = "crash_guard";

/* RTC_NOINIT survives a reset but is uninitialised after power-on, so the
 * magic word is what tells a real streak from whatever happened to be in that
 * RAM at cold boot. */
#define CRASH_GUARD_MAGIC   0xC0FFEE5AU
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_streak;

static int s_streak_at_boot;
static bool s_healthy;

static void healthy_cb(void *arg)
{
    crash_guard_mark_healthy();
}

void crash_guard_init(void)
{
    if (s_magic != CRASH_GUARD_MAGIC) {     /* cold boot, or first ever */
        s_magic  = CRASH_GUARD_MAGIC;
        s_streak = 0;
    }

    esp_reset_reason_t reason = esp_reset_reason();

    /* Only genuine faults count. Our own esp_restart() reports ESP_RST_SW, so
     * a reboot from the web page, an OTA or the healing path below can never
     * inflate the streak that triggers healing. */
    bool crashed = (reason == ESP_RST_PANIC)    ||
                   (reason == ESP_RST_TASK_WDT) ||
                   (reason == ESP_RST_INT_WDT)  ||
                   (reason == ESP_RST_WDT);

    if (crashed) {
        s_streak++;
        ESP_LOGE(TAG, "boot follows a crash (reason %d), streak %lu",
                 (int)reason, (unsigned long)s_streak);
    } else if (reason != ESP_RST_SW) {
        s_streak = 0;                        /* power cycle: a clean slate */
    }
    s_streak_at_boot = (int)s_streak;

    if (s_streak >= CRASH_LOOP_THRESHOLD) {
        /* The most likely cause of a boot that panics every time is the
         * network it is trying to join, and that is also the one thing the
         * user can fix. Drop the credentials and come back as a setup AP
         * rather than crash-looping silently forever. */
        ESP_LOGE(TAG, "%lu crash-boots in a row — clearing WiFi and returning to setup",
                 (unsigned long)s_streak);
        s_streak = 0;
        nvs_config_clear_wifi();
        return;                              /* boot continues; no creds -> portal */
    }

    if (s_streak > 0) {
        /* Give this boot a chance to prove itself. */
        const esp_timer_create_args_t args = {
            .callback = healthy_cb, .name = "crash_ok",
        };
        esp_timer_handle_t t;
        if (esp_timer_create(&args, &t) == ESP_OK)
            esp_timer_start_once(t, (uint64_t)CRASH_GUARD_HEALTHY_SEC * 1000000ULL);
    }
}

void crash_guard_mark_healthy(void)
{
    if (s_healthy) return;
    s_healthy = true;
    if (s_streak != 0) {
        ESP_LOGI(TAG, "boot looks healthy — crash streak cleared");
        s_streak = 0;
    }
}

int crash_guard_streak(void) { return s_streak_at_boot; }
