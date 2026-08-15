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
        /* Same reasoning as the mule, but the miner has no user to onboard:
         * it forgets the card credentials and the mule re-pushes them with
         * set_config on its next boot. So healing here costs one round trip,
         * not a re-provision. */
        ESP_LOGE(TAG, "%lu crash-boots in a row — clearing ezShare credentials",
                 (unsigned long)s_streak);
        s_streak = 0;
        nvs_config_erase_all();
        return;
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
