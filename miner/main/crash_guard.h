#pragma once

/**
 * @file crash_guard.h
 * @brief Self-heal from a boot that panics every time.
 *
 * The miner is even less reachable than the mule: no console, no network of
 * its own, and only the link to talk through. Count consecutive crash-boots,
 * and once the count says the problem will not fix itself, forget the ezShare
 * credentials. The mule re-pushes them on its next boot, so healing costs one
 * round trip rather than a re-provision.
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
