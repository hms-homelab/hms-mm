#pragma once

/**
 * @file crash_guard.h
 * @brief Self-heal from a boot that panics every time.
 *
 * A device wedged in a crash loop behind a CPAP machine is a device nobody can
 * reach: no console, no network, and the one setting most likely to be at
 * fault (bad or changed WiFi credentials) is exactly what keeps it from coming
 * back. So count consecutive crash-boots, and once that count says the problem
 * is not going to fix itself, clear the credentials and return to onboarding.
 *
 * The counter lives in RTC memory so it survives a panic and a software reset
 * but not a power cycle. That is the behaviour we want: unplugging the device
 * is the user's own reset, and should not inherit a streak from days ago.
 */

#include <stdbool.h>

/**
 * @brief Inspect the reset reason and act on a crash streak.
 *
 * Call early in app_main, after NVS is up (it may need to clear credentials)
 * and before anything that could itself crash. Starts a timer that clears the
 * streak once this boot has stayed up long enough to count as healthy.
 */
void crash_guard_init(void);

/**
 * @brief Declare this boot healthy now, without waiting for the timer.
 *
 * For milestones that prove more than uptime does, such as the HTTP server
 * actually serving.
 */
void crash_guard_mark_healthy(void);

/** Consecutive crash-boots before this one, for reporting. */
int crash_guard_streak(void);
