/*
 * Host test for the failed-card-request message.
 *
 *   cc -I miner/main miner/test/host/test_ez_error.c miner/main/ez_error.c -o /tmp/t && /tmp/t
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ez_error.h"

static int failures = 0;

static void expect(int status, const char *name, size_t n, const char *want)
{
    char buf[128];
    memset(buf, 'X', sizeof(buf));
    ez_failure_message(status, name, buf, n);
    if (strcmp(buf, want) != 0) {
        printf("FAIL status=%d name=%s n=%zu: got \"%s\", want \"%s\"\n",
               status, name ? name : "(null)", n, buf, want);
        failures++;
    }
}

int main(void)
{
    /* The card answered: its status is the reason. */
    expect(404, "ESP_ERR_HTTP_BASE", 128, "ezShare request failed: card answered HTTP 404");
    expect(500, NULL, 128, "ezShare request failed: card answered HTTP 500");

    /* It never answered: the transport's own error. */
    expect(0, "ESP_ERR_HTTP_CONNECT", 128, "ezShare request failed: ESP_ERR_HTTP_CONNECT");
    expect(0, "ESP_FAIL", 128, "ezShare request failed: ESP_FAIL");
    expect(0, NULL, 128, "ezShare request failed: unknown");

    /* A short buffer is cut, never overrun, and still terminated. */
    expect(404, NULL, 8, "ezShare");

    /* A zero-length buffer is left alone. */
    char untouched[4] = "abc";
    ez_failure_message(404, NULL, untouched, 0);
    if (strcmp(untouched, "abc") != 0) { printf("FAIL n=0 wrote\n"); failures++; }

    if (failures) { printf("%d failure(s)\n", failures); return EXIT_FAILURE; }
    printf("ez_error: all passed\n");
    return EXIT_SUCCESS;
}
