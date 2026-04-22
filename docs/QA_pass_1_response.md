# QA Pass 1 — Response (2026-04-22)

Response to `QA_pass_1_2026-04-22.md` static review findings. Based on commit `db651fd` (post-pull).

## Fixed

| # | Finding | Fix |
|---|---------|-----|
| 1 | Proxy mutex held across full UART roundtrip | Increased mutex wait from 100ms to 65s (`PROXY_REQ_TIMEOUT_MS + 5000`) — queues instead of rejecting |
| 3 | DNS QNAME parse stack overflow | Added label-length bounds check in `captive_portal.c` DNS loop |
| 4 | strncpy without NUL on proxy_path | Added explicit `proxy_path[sizeof(...)-1] = '\0'` after strncpy |
| 5 | AP SSID not NUL-terminated | Changed to `strncpy(..., size-1)` + explicit NUL in captive portal |
| 8/17 | JSON parse failure loop / no rate limit | Added parse failure counter (max 10), returns 502 on sustained failures |
| 9 | Shared static buffers across concurrent requests | Changed `static` buffers to stack-local in `proxy_forward_request` — mutex serializes anyway but stack is safer |
| 12 | Unbounded realloc on ezShare response | Capped HTTP response buffer at 256KB in `ezshare_client.c` |
| 14 | esp_restart before UART flush | Added `uart_wait_tx_done()` before restart in miner `handle_set_config` |
| 20 | WiFi strncpy no NUL guarantee | Added explicit NUL terminator for SSID and password in `wifi_manager.c` |
| 21 | URL decode strtol accepts invalid hex | Added `endptr` validation — invalid `%XX` sequences are passed through literally |
| 24 | Silent fallback to default ezShare creds | Changed log from `ESP_LOGI` to `ESP_LOGW` to make fallback visible |

## Already fixed / false positives

| # | Finding | Status |
|---|---------|--------|
| 15 | NVS uninitialized stack buffer | `read_str()` sets `buf[0] = '\0'` on failure (line 17) — buffer is always initialized |
| 19 | mbedtls_base64_encode on zero-length input | `b64_len` is set to 0, `malloc(1)` succeeds, encode writes nothing — well-defined behavior |

## Skipped — acceptable risk

| # | Finding | Reason |
|---|---------|--------|
| 2 | Path traversal / unauth LAN exposure | The proxy forwards to ezShare which only serves its own SD card (`A:`). No filesystem to traverse. LAN access to CPAP data is the device's intended purpose |
| 6 | Silent query-param truncation | `MAX_PATH` (256) exceeds any valid ezShare path. Truncation would produce a 404 from ezShare, not data corruption |
| 7 | Base64 decode output unchecked | `mbedtls_base64_decode` returns the actual decoded length in `decoded_len`; `httpd_resp_send_chunk` uses that length, not the buffer size |
| 10 | s_req_id++ not atomic | Proxy mutex serializes all requests — only one handler runs the increment at a time |
| 11 | Race on last_proxy_time_us | Written by scanner_task (single task), read by `check_idle_disconnect()` in the same task loop. No cross-task race |
| 13 | Per-chunk UART timeout 60s, no overall cap | Miner sends data or disconnects within timeout. A stuck miner would hit the per-chunk timeout and return 504 |
| 16 | cJSON NULL-root not rechecked | Line 220: `if (!root) break;` — NULL is checked immediately after parse |
| 18 | HTTP client init failure | `esp_http_client_init` returns NULL on failure; line 199 checks `if (!client)` and returns |
| 22 | parse_field uses strstr prefix match | The field check at line 148 (`pos[nlen] != '='`) prevents `user` matching `user_id=` — only exact `name=` matches pass |
| 23 | uart_send_json returns success on send failure | The mule proxy checks UART timeout on the receive side. A silently dropped send triggers a receive timeout → 504 to client |
| 25 | Range header parses only single range | Multi-range is rare and unsupported by ezShare itself. Single range covers the CPAPdash app's needs |
| 26 | Architecture drift | README updated in previous commit |
| 27 | Idle disconnect race | Window is sub-second at the 5-minute boundary. First chunk failure triggers a reconnect on the next proxy request |
