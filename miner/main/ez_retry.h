/**
 * @file ez_retry.h
 * @brief The ezShare join retry schedule.
 *
 * Pure (no ESP-IDF headers) so it can be tested on the host:
 *   cc -I miner/main miner/test/host/test_ez_retry.c miner/main/ez_retry.c -o /tmp/t && /tmp/t
 */

#ifndef EZ_RETRY_H
#define EZ_RETRY_H

#include <stdint.h>

#define EZ_RETRY_SPACED_MS     5000u    /* tries 1..EZ_RETRY_SPACED_TRIES */
#define EZ_RETRY_SPACED_TRIES  10u
#define EZ_RETRY_MAX_MS        300000u  /* exponential phase cap, 5 min */

/**
 * @brief Delay before the n-th retry after a failed join or a drop (n >= 1).
 *
 * 5 s for the first 10 tries, then 10, 20, 40, 80, 160 s, then 300 s for every
 * try after that. n = 0 is treated as n = 1. Never gives up.
 */
uint32_t ez_retry_delay_ms(uint32_t n);

#endif /* EZ_RETRY_H */
