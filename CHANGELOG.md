# Changelog

Version format: `YYYY.MINOR.PATCH`. The mule and the miner version
independently — a release often touches one board and not the other, and either
can be updated without the other, so in the field they legitimately differ.
`/api/status` reports both (`fw` and `miner_fw`).

## [Unreleased]

### Breaking — flash both boards together

- **Partition table now has two OTA slots.** `factory` is replaced by
  `otadata` + `ota_0` + `ota_1`, which is what makes firmware updates possible
  at all. A flash layout cannot be changed over the air, so moving to this
  release needs **one USB flash at offset 0x0** on each board:
  ```
  esptool.py --chip esp32c3 -p PORT write_flash 0x0 mule-VERSION-merged.bin
  esptool.py --chip esp32c3 -p PORT write_flash 0x0 miner-VERSION-merged.bin
  ```
  The `*-merged.bin` images published with each release contain the bootloader,
  the partition table and the app together. Every update after this one can go
  over the network. Note the app partition shrinks from 0x1F0000 to 0x1E0000,
  because two slots and otadata now share the same 4 MB.
- **UART baud 115200 to 921600.** The boards must agree on the baud, so a unit
  running a mix of old and new firmware will not talk at all. Reflash the pair.
  Raw link capacity goes from ~11.5 KB/s to ~92 KB/s; after base64's 4/3 tax
  that is roughly 8.6 KB/s to 69 KB/s of payload. Backed off in one place
  (`UART_BAUD_RATE`) if a rig shows framing errors.
- **`proxy_chunk` gained a `c` field** (CRC32 of the decoded bytes). Frames
  without it are still accepted, so the change itself is not what forces the
  reflash — the baud is.

### Fixed

- **Removed the async HTTP worker pool.** Added in 2026.0.5 and never hardware
  tested; the upstream project it was ported from deleted the same pattern five
  days later because it crash-looped the C3 (~10 KB free heap, versus ~55 KB
  stable once removed). The worker machinery is heap-fragile on a chip that is
  already tight, and it was pretending there was concurrency to exploit: the
  link is one wire to one single-threaded miner, and ezShare itself is
  single-connection with keep-alive disabled. Handlers run synchronously again,
  serialised by `s_proxy_mutex` exactly as before 2026.0.5, and the worker
  task's ~4 KB comes back.
- **A busy proxy no longer parks the whole HTTP server.** With the worker pool
  gone, handlers run on the single httpd task, and the mutex acquire timeouts
  (35 s for `/dir` and `/download`, 125 s for an O2Ring download) would have
  blocked every other route — including `/api/status` — behind one transfer.
  Acquiring now fast-fails after `PROXY_LOCK_ACQUIRE_MS` (3 s) with a `503` and
  `Retry-After`. A transfer that already holds the lock still keeps it as long
  as it genuinely needs.
- **A lost chunk no longer produces a silently corrupt file.** A `seq` gap was
  logged as a warning and otherwise ignored, so the client received a `200`
  with a hole in it. Both streaming paths now abort the transfer instead.
- **The O2Ring download path ignored `seq` entirely and skipped past a failed
  base64 decode**, quietly dropping a chunk and returning a truncated `.vld`
  that still looked like a success. It now aborts, and a client disconnect
  breaks the loop rather than streaming into a dead socket.
- **Chunk corruption is now detectable at all.** There was no integrity check
  anywhere on the link — no parity, no checksum, and a successful base64 decode
  proves nothing, because dropping bytes from a frame still decodes cleanly
  into shorter garbage. Chunks now carry a CRC32 of the decoded bytes
  (`esp_rom_crc32_le`, a ROM routine, so effectively free).
- **The mid-stream abort peek no longer eats other messages.** It read up to
  256 bytes of the RX buffer and discarded them on a bare substring match for
  `proxy_abort`. That was only safe while an abort was the sole thing the mule
  could send mid-stream; it now parses the frame to confirm, and leaves
  anything else in place to be delivered normally.
- **The miner had no UART TX mutex** (the mule always had one), which was fine
  only while `scanner_task` was the sole transmitter. Added ahead of the task
  split, so two interleaved writes can never produce one unparseable line plus
  one lost frame.
- **A provisioned mule could never rejoin its network.** `nvs_config_has_wifi()`
  probed the stored SSID with a 4-byte buffer, and `nvs_get_str` returns
  `ESP_ERR_NVS_INVALID_LENGTH` rather than `ESP_OK` when the buffer is too
  small, so any SSID of four characters or more was reported as absent. With
  the stock `config.h` the Kconfig fallback is excluded by design, so the boot
  path fell through to the captive portal on **every** boot: saved credentials
  were written correctly and then never used. `has_ezshare()` had the identical
  bug, which is why the mule never re-sent the card credentials to the miner.
  Both now ask NVS for the required length instead of guessing at it.
- **A mule reboot no longer restarts the miner.** The mule sends `set_config`
  on every boot and the miner restarted unconditionally to apply it — so a mule
  crash-and-recover took the miner down too, potentially mid-transfer. It now
  restarts only when the credentials actually changed, which is the only case
  where a restart does anything.
- **`config_ack` was sent by the miner and read by nobody.** A miner that never
  received its configuration was indistinguishable from one that did. The mule
  now waits for it and logs the outcome.
- **`/api/status` reported fabricated values** — a hardcoded `"mqtt":false`
  (there is no MQTT client anywhere in this project) and `"o2ring":false`
  regardless of the real state. Replaced with a real payload: `fw`, `miner_fw`,
  `wifi`, `uptime`, `free_heap`, `largest_block`, `min_free`, and the `ezshare`
  diagnostics block. `largest_block` matters more than `free_heap` on a C3,
  where fragmentation rather than total free is what fails an allocation.

### Changed

- **UART receive reads in blocks instead of one byte per driver call.** The old
  loop called `uart_read_bytes` per byte — ~11k calls/second at the old baud,
  and ~92k at the new one. A line assembler now buffers across calls, which is
  what makes block reads safe when a read can return a partial frame, a whole
  frame, or several frames plus a fragment. An unterminated buffer-filling
  frame is dropped to resync rather than wedging the link forever.
- RX ring buffers 8 KB to 16 KB (~178 ms of slack at 921600, with no hardware
  flow control available on a two-wire link).
- **Each board owns its version.** `version.h` moved from the repo root into
  `mule/main/` and `miner/main/`. The root copy was included by nothing and had
  drifted to `2026.0.5` while both boards independently defined `2026.0.6`.

### Added

- **Firmware updates, over the network or by upload.** `POST /api/ota` takes
  `{"target","url"}` and the device fetches and installs it itself;
  `POST /api/update/{mule,miner}` takes the raw `.bin` in the body and needs no
  network at all. The miner is updated through the mule over the link, since it
  has no network of its own. `GET /api/update` is a page for both, with live
  progress. Nothing polls for updates: the device contacts a server only when
  you hand it a URL.
  `https://` is verified against the bundled root certificates
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`, matching the sibling project) because
  this installs executable code and an unverified connection would let anyone
  on the network path choose the firmware.
- **Rollback protection.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` on both
  boards, so a new image boots on probation and the bootloader reverts unless
  the app confirms itself. The mule confirms once WiFi and the server are up.
  The miner confirms on the first frame it successfully decodes from the mule,
  and reboots unconfirmed after 180 s if it never manages one: the link is its
  only route in, so an image that boots but cannot talk is otherwise
  unrecoverable without opening the case. A decoded frame is the proof; noise
  on the wire must never count.
- **An update locks out conflicting work.** Every route that drives the miner
  or changes configuration answers `503` with `Retry-After` while an update is
  running, and the miner refuses anything that is not an OTA frame for the
  duration. Both halves are needed: the HTTP gate does nothing about frames
  already on the wire, and the miner gate turns a well-formed request into
  silence rather than a diagnosable error.
- **A device page and a local control API.** Point a browser at the mule and you
  get status, the SD card links, the O2 Ring switch, recent logs and the
  maintenance actions. New `control_server.c` owns the single httpd and mounts
  the existing data routes onto it, so everything shares one server and one
  link lock. New endpoints: `GET /`, `POST /api/reboot` (mule, miner, or both),
  `POST /api/reset`, `POST /api/config`, `GET /api/logs`.
  Verified by driving the real page in a browser against a mocked device across
  three states (card missing, card healthy, fresh boot with nothing failed
  yet), with no console errors.
- **`GET /api/logs`** and `log_ring.c`: an 8 KB ring of recent log lines
  captured through `esp_log_set_vprintf`, with the ANSI colour codes stripped
  so the output is readable in a browser or a pasted support log. The console
  keeps its colours. Until now the only way to see what a unit was doing was a
  USB cable, which is not a reasonable thing to ask of a box mounted behind a
  CPAP machine.
- **Control messages on the link**: `version_req`/`version_resp`, `reboot`,
  `reset`, `o2_state_req`/`o2_state_resp` and `o2_set_enabled`. `reboot` leaves
  the miner's config alone; `reset` erases it and lets the mule re-push
  credentials on its next boot. Mule-side exchanges live in `miner_link.c` and
  hold the link across send-and-reply, so a control request issued while a
  transfer is running cannot steal its frames.
- **ezShare diagnostics on every miner error** (`assoc`, `rssi`, `reason`).
  Previously every failure reached the mule as an undifferentiated `502`;
  reason 201 (card off/asleep/out of range) and 202 (wrong card password) are
  entirely different problems and are now distinguishable without a serial
  cable. Surfaced under `ezshare` in `/api/status`.
- **The miner now logs its WiFi disconnect reason.** It captured the code and
  printed none of it, so every cause looked the same in a log.
- **Release CI** (`.github/workflows/release.yml`). On a `v*` tag: builds both
  boards, publishes app images, bootloaders, partition tables, `*-merged.bin`
  single-file images flashable at `0x0`, and `SHA256SUMS.txt`. Artefacts are
  named for each board's own version. Fails if the tag matches neither board's
  version, which catches tagging a release without bumping anything. Needs no
  secrets beyond `GITHUB_TOKEN`.

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
