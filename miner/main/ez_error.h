/**
 * @file ez_error.h
 * @brief The message a failed card request sends back to the mule.
 *
 * Pure (no ESP-IDF headers) so it can be tested on the host:
 *   cc -I miner/main miner/test/host/test_ez_error.c miner/main/ez_error.c -o /tmp/t && /tmp/t
 */

#ifndef EZ_ERROR_H
#define EZ_ERROR_H

#include <stddef.h>

/**
 * @brief Write the reason a card request failed into buf.
 *
 * The mule hands this text to the client as the body of its 502, and logs it.
 * It used to say only "ezShare request failed", so a card answering 404, a
 * card refusing the connection and a link dropping mid-read all looked the
 * same from outside the miner.
 *
 * @param http_status The card's HTTP status, or 0 if it never answered.
 * @param err_name    esp_err_to_name() of the failure, used when there is no
 *                    status. NULL reads as "unknown".
 */
void ez_failure_message(int http_status, const char *err_name, char *buf, size_t n);

#endif /* EZ_ERROR_H */
