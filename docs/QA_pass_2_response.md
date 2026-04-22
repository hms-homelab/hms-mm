# QA Pass 2 — Response (2026-04-22)

Response to `QA_pass_2_2026-04-22.md` findings. Based on commit `1040e25`.

## Pushback verdicts — agreed

All three withdrawals (#2, #11, #22) are correct. LAN-trusted, single-task loop, boundary checks present.

## Pass-1 fix caveats acknowledged

- **Mutex timeout (#1 caveat):** 65s timeout is intentional — it's the maximum wait, not a deadlock. A stuck miner releases after timeout, not forever. Worker starvation (#3) is a design tradeoff addressed below.
- **Realloc cap (#12 caveat):** Second path (`file_list_add`) missed — fixed in this pass.

## Fixed

| # | Finding | Fix |
|---|---------|-----|
| 1 | File list realloc unbounded | Capped `ezshare_file_list_add` at 4096 entries in `ezshare_client.c` |
| 2 | Static `cr_hdr` shared across requests | Changed from `static char` to stack-local `char` in `file_server.c` |
| 5 | proxy_chunk seq/id not validated | Added `id` field check on `proxy_chunk` messages — stale chunks from wrong request are skipped |
| 6 | proxy_meta no req_id match | Added `id` field check on `proxy_meta` messages — stale meta from wrong request is skipped |
| 14 | Base64 malloc per chunk | Replaced per-call malloc/free with static buffer sized to `FILE_CHUNK_SIZE * 4/3 + 8` |

## Already safe / false positives

| # | Finding | Status |
|---|---------|--------|
| 4 | proxy_req overwrites in-flight request | State machine prevents this — `proxy_req` only accepted in `SCANNER_IDLE` state (line 212). While in `SCANNER_PROXY`, UART messages are not processed. Can't overlap |
| 10 | Range end not validated against file size | Forwarded to ezShare which is authoritative on file sizes. Oversized range returns 200 + full file, not corruption |
| 12 | Wrong password boot behavior | Confirmed: WiFi connect fail → `captive_portal_start()`. No retry loop, no lockout. Falls to portal immediately |
| 13 | url_decode reads past terminator | Already handled by pass 1 `endptr` validation — trailing `%A` produces `endptr != hex + 2`, so the `%` is passed through literally |

## Skipped — acceptable risk

| # | Finding | Reason |
|---|---------|--------|
| 3 | 65s mutex → worker starvation | Fundamental tradeoff of single-UART proxy design. The 65s timeout is the safety valve — it always releases. A supervisor task would add significant complexity for a home-use device. Most browser requests complete in <2s; starvation requires a crashed miner + multiple concurrent clients |
| 7 | NVS double-submit race | Captive portal is a one-time setup page served to a single phone. Two concurrent submissions from different devices during initial setup is effectively impossible |
| 8 | UART polling starves handler | `uart_read_bytes` yields to FreeRTOS scheduler. WDT is 10s, chunk timeout is 60s. No real starvation — the scheduler ensures other tasks get CPU time |
| 9 | 1-byte keepalive defeats timeout | Miner protocol doesn't send keepalives. This requires a compromised miner, which is out of threat model for home-use |
| 11 | Realloc comment | Code is correct. Not adding a comment for a well-understood pattern |
| 15 | dir_param forwarded raw to miner | LAN-trusted, ezShare scoped. ezShare only serves `A:` paths |
