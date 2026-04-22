# QA Pass 3 — Response (2026-04-22)

Response to `QA_pass_3_2026-04-22.md` findings. Based on commit `157812e`.

## Pushback verdicts — agreed

All three withdrawals (#4, #8, #13 from pass 2) are correct. State machine prevents proxy_req overlap, UART yields properly, url_decode bounds check exists.

## Pass-2 fix verification — agreed

All pass-2 fixes verified clean. Base64 buffer math confirmed correct.

## Fixed

| # | Finding | Fix |
|---|---------|-----|
| 1 | UART send has no mutex — boot-time race | Added `SemaphoreHandle_t uart_mutex` to `uart_handler.c`. Created in `uart_handler_init()`, taken/given around the write+newline+flush sequence in `uart_send_json()`. Prevents interleaved JSON from mule_task and HTTP proxy handlers |
| 2 | httpd_start failure silently degrades | Added `else` branch: `ESP_LOGE` + `esp_restart()` on HTTP server start failure. Device no longer runs in silent degraded mode |
| 3 | proxy_chunk seq not validated | Added `expected_seq` counter in `proxy_forward_request`. Logs warning on mismatch. Increments after each successful chunk. Defense-in-depth against miner-side bugs |
| 4 | HTTP status passed through without range check | Clamped to 100-599 range; out-of-range values default to 200 |

## Skipped — acceptable risk

| # | Finding | Reason |
|---|---------|--------|
| 5 | Content-Length vs chunk total mismatch | Chunked transfer encoding doesn't declare Content-Length to the HTTP client. The `cl` field from proxy_meta is used only for Range/Content-Range headers, not for the chunked body. Client detects truncation via stream termination |
| 6 | cJSON nesting depth | LAN-trusted, miner runs our firmware. Default cJSON recursion limit (~200) is sufficient. Not configurable without patching the library |

## Overall QA summary

Across three passes:
- **Pass 1:** 25 findings → 11 fixed, 4 already safe, 10 acceptable risk
- **Pass 2:** 12 findings → 5 fixed, 4 already safe, 3 acceptable risk
- **Pass 3:** 6 findings → 4 fixed, 2 acceptable risk

Total: 43 findings reviewed, 20 fixed, 8 false positives/already safe, 15 acceptable risk. Codebase is in good shape for domestic use.
