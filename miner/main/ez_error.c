#include "ez_error.h"

#include <stdio.h>

void ez_failure_message(int http_status, const char *err_name, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    if (http_status >= 100)
        snprintf(buf, n, "ezShare request failed: card answered HTTP %d", http_status);
    else
        snprintf(buf, n, "ezShare request failed: %s", err_name ? err_name : "unknown");
}
