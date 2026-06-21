# Changelog

Version format: `YYYY.MINOR.PATCH`

## [2026.0.5] - 2026-06-21

### Fixed
- **HTTP server wedged under concurrent clients / a slow card.** `esp_http_server` runs a single task, so a blocking handler (`/dir`, `/download`, or an `o2ring` request waiting up to ~2 min on BLE) stalled every other request, including the static `/api/status`. Adopted the ESP-IDF async-handler pattern: the UART-backed handlers now run on a worker pool (`http_async`), freeing the httpd task so the server stays responsive. With one UART link and one miner scanner, the pool is sized to 1 worker (proxy work is serialized to what the miner can actually handle); excess concurrent requests get an immediate "Server busy" 503 instead of piling up.
- **Proxy could hold its mutex forever.** `proxy_forward_request`'s per-frame UART timeout reset on every successful read, so a flood of stale-`req_id` frames (e.g. after a client disconnect) could spin the loop indefinitely while holding `s_proxy_mutex`. Added a no-progress stall-deadline that resets only on a frame for the active request, so the mutex is always released.
- **Client-disconnect abort.** When the HTTP client drops mid-stream, the mule now sends a `proxy_abort` and drains the miner's leftover chunks (capped by `PROXY_ABORT_DRAIN_MS`), and the miner stops its ezShare fetch immediately: it peeks the UART RX buffer between chunks (`uart_check_proxy_abort`) and bails out of `ezshare_raw_get_range`. Stops a dead request from pulling the whole file off the card and colliding with the next request's response.
- **`PROXY_REQ_TIMEOUT_MS` 60s to 30s.** Per-frame margin for a slow/flaky ezShare card, without the old wedge risk now that the async pool keeps the server responsive.

### Added
- **`miner/ble_active` NVS flag (default off).** Bringing up the O2Ring BLE disconnects the ezShare WiFi link (shared radio on the C3), which interrupts CPAP data collection. The miner now refuses `o2ring` requests with "BLE disabled" unless `ble_active=1` is set in NVS, so the data link is never dropped unexpectedly. Set the flag (then reboot) to enable BLE for development.

### Notes
- Built clean and code-reviewed, but **not yet hardware-tested** (no UART rig available at release time); hardware validation to follow.

## [2026.0.4] - 2026-05-31

### Fixed
- **httpd task stack overflow in file/O2Ring download handlers.** `handle_download` and `handle_o2ring_files` (download mode) each declared `char uart_buf[PROXY_UART_BUF_SIZE]` (8 KB) + `uint8_t decode_buf[PROXY_CHUNK_SIZE]` (4 KB) as **stack** locals — ~12 KB in the 8 KB httpd task (`main.c` `stack_size = 8192`), guaranteed to overflow and corrupt memory / reset the mule on any download. Moved both buffers (and the 4 KB `uart_buf` in `wait_o2ring_json_response`) to `static` storage; they're only touched while holding `s_proxy_mutex`, so one-at-a-time access keeps them safe. The O2Ring `/status`, `/files` (list), and `/live` endpoints were intact — not removed.

## [2026.0.3] - 2026-05-24

### Added — O2Ring BLE oximetry support
- **Miner BLE client**: ported Wellue O2Ring GATT client from the upstream C3 project (`o2ring_ble.c/h`) — Viatom protocol with CRC-8 framing, 128-bit service/characteristic UUIDs
- **Scanner O2Ring state**: new `SCANNER_O2RING` state handles `o2ring_req` UART messages with 4 sub-commands: `status`, `files`, `live`, `download`
- **HTTP endpoints on mule**: `/o2ring/status`, `/o2ring/files`, `/o2ring/files?name=FILE.vld`, `/o2ring/live` — proxied to miner over UART
- **WiFi/BLE radio sequencing**: miner disconnects WiFi before BLE operations and vice versa (ESP32-C3 shared radio)
- **Lazy BLE init**: BLE stack only loads on first `o2ring_req`, saving ~50KB heap when O2Ring is not used
- **File downloads**: O2Ring `.vld` files streamed via existing `proxy_meta` + `proxy_chunk` UART framing
- **BLE sdkconfig**: enabled Bluedroid stack for miner (`CONFIG_BT_ENABLED`, `CONFIG_BT_BLUEDROID_ENABLED`)
- Spec document: `docs/o2ring-ble-integration.md`

### Changed
- `handle_status()` now includes `"o2ring"` field in `/api/status` response
- URI registration uses `sizeof(uris)/sizeof(uris[0])` instead of hardcoded count
- Version bumped to 2026.0.3

## [2026.1.0] - 2026-04-21

### Changed — Architecture: batch collector to pure proxy
- **Mule rewritten as HTTP proxy**: no more file caching, batch collection, or base64 decoding — every `/dir` and `/download` request proxies through UART to the miner in real time
- **Miner simplified to proxy-only**: removed LISTING/DOWNLOADING/SENDING states, scanner is now IDLE or PROXY
- **`mule_task.c`**: stripped from 190-line state machine (IDLE/REQUESTING/RECEIVING/DECODING/ERROR) to boot-time ezShare config sender + idle loop
- **`file_server.c`**: replaced file-cache-based serving with `proxy_forward_request()` — UART JSON protocol with base64-encoded chunks
- **`file_cache.c`/`file_cache.h`**: removed entirely (no more in-memory caching)
- **Mule state machine**: 5 states (IDLE/REQUESTING/RECEIVING/DECODING/ERROR) collapsed to MULE_IDLE
- **Miner state machine**: 6 states collapsed to SCANNER_IDLE + SCANNER_PROXY
- **Config cleanup**: removed MAX_FILE_SIZE, FILE_CACHE_MAX_FILES, MULE_COLLECTION_INTERVAL_SEC; added PROXY_CHUNK_SIZE, PROXY_UART_BUF_SIZE, PROXY_IDLE_TIMEOUT_MS
- **UART RX buffer**: 64 KB down to 8 KB (no more full-file buffering)
- **Mule task stack**: 12 KB down to 8 KB
- Boot delay: 30s down to 5s (no collection cycle to wait for)

### Added — HTTP Range requests
- **`ezshare_raw_get_range()`**: HTTP GET with optional `Range` header, reports status/content-length via out-params
- **`MSG_PROXY_META` (0x0C)** UART message: miner sends HTTP status, content-length, and total size before data chunks
- **`spi_proxy_meta_t`** wire struct: req_id, http_status (200/206), content_length, total_size
- **`parse_range_header()`** on mule: parses `Range: bytes=START-END` from incoming HTTP requests
- **`Accept-Ranges: bytes`** and **`Content-Range`** headers in mule HTTP responses for 206 Partial Content
- **`proxy_forward_request()`** with range params, sets 206 status and Content-Range from miner META
- `esp_timer` dependency added to miner CMakeLists

### Changed — Protocol
- **`spi_proxy_req_t`** wire format: added `range_start` and `range_end` fields (breaking wire protocol change)
- **`ezshare_stream_file()` resume**: uses HTTP Range header instead of reading and discarding bytes
- HTTP status validation accepts both 200 and 206 throughout

## [2026.0.0] - 2026-04-05

### Added
- Dual ESP32-C3 miner/mule architecture with UART JSON protocol
- Miner: connects to ezShare WiFi, downloads files, sends to mule via UART
- Mule: receives files via UART, caches in memory, serves via HTTP
- Captive portal on mule with DNS hijack for WiFi setup
- Single setup form collects home WiFi and ezShare credentials
- Mule sends ezShare creds to miner via UART on save, both reboot
- NVS credential persistence on both devices with Kconfig fallback
- ezShare-compatible HTTP API (/dir, /download, /api/status)
- Custom partition table (2MB app partition on 4MB flash)
