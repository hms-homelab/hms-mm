# Changelog

Version format: `YYYY.MINOR.PATCH`

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
