/**
 * @file ez_retry.c
 * @brief The ezShare join retry schedule.
 */

#include "ez_retry.h"

uint32_t ez_retry_delay_ms(uint32_t n)
{
    if (n <= EZ_RETRY_SPACED_TRIES) return EZ_RETRY_SPACED_MS;

    /* Exponential phase: doubles from 5 s, so the first value is 10 s. Stop
     * doubling at the cap instead of shifting by n, which overflows. */
    uint32_t delay = EZ_RETRY_SPACED_MS;
    for (uint32_t i = EZ_RETRY_SPACED_TRIES; i < n; i++) {
        delay *= 2;
        if (delay >= EZ_RETRY_MAX_MS) return EZ_RETRY_MAX_MS;
    }
    return delay;
}
