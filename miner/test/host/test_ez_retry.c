/*
 * Host test for the ezShare join retry schedule.
 *
 *   cc -I miner/main miner/test/host/test_ez_retry.c miner/main/ez_retry.c -o /tmp/t && /tmp/t
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ez_retry.h"

static int failures = 0;

static void expect(uint32_t n, uint32_t want)
{
    uint32_t got = ez_retry_delay_ms(n);
    if (got != want) {
        printf("FAIL n=%u: got %u, want %u\n", n, got, want);
        failures++;
    }
}

int main(void)
{
    /* Ten tries, 5 s apart. */
    for (uint32_t n = 1; n <= 10; n++) expect(n, 5000);

    /* Then exponential from 5 s, capped at 5 min. */
    expect(11, 10000);
    expect(12, 20000);
    expect(13, 40000);
    expect(14, 80000);
    expect(15, 160000);
    expect(16, 300000);

    /* Stays at the cap, and a huge n does not overflow into a short delay. */
    expect(17, 300000);
    expect(100, 300000);
    expect(UINT32_MAX, 300000);

    /* n = 0 behaves as the first try. */
    expect(0, 5000);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ez_retry: all passed\n");
    return 0;
}
