# O2Ring BLE Integration for hms-mm

**Status:** Draft
**Date:** 2026-05-24
**Affects:** hms-mm (miner + mule firmware)
**Reference:** cpapdash-push-c3 O2Ring implementation (`miner/main/o2ring_ble.c`)

## Problem

Users want to collect Wellue O2Ring oximetry data through the hms-mm bridge alongside CPAP data from the ezShare WiFi SD card. The O2Ring is a BLE peripheral — it requires a BLE central to connect, read live sensor data (SpO2, HR), and download stored .vld session files. hms-mm currently has no BLE capability.

GitHub issue: hms-homelab/hms-cpap#2 — user `ejukated` plans to use the hms-mm bridge with an RPi travel router and expects O2Ring BLE support on the same ESP32-C3.

## Design

Add O2Ring BLE to the **miner** ESP32-C3. The miner becomes a dual-source collector: ezShare WiFi SD (WiFi) and O2Ring (BLE). The mule proxies O2Ring HTTP requests to the miner over UART, same pattern as ezShare file proxying.

### Why the miner, not the mule

- The mule runs home WiFi + HTTP server — adding BLE would contend for heap (~50KB BLE + ~30KB WiFi + HTTP overhead).
- The miner is idle between proxy requests — plenty of free heap for BLE.
- WiFi and BLE share the ESP32-C3 radio. The miner can sequence them: WiFi off -> BLE on -> read O2Ring -> BLE off -> WiFi available again. No concurrent radio use.
- Same rationale as cpapdash-push-c3 SDD-002.

### Architecture

```
O2 Ring (BLE peripheral)
   |
   v  BLE GATT (Viatom protocol)
Miner C3 (BLE central + WiFi STA)
   |
   v  UART JSON (115200 baud)
Mule C3 (WiFi STA + HTTP server :80)
   |
   v  HTTP
Browser / hms-cpap / curl
```

### Radio sequencing on the miner

The miner uses WiFi and BLE **sequentially, never concurrently**:

```
IDLE (no radio)
  |
  v  proxy_req from mule
WiFi ON -> fetch from ezShare -> WiFi OFF
  |
  v  o2ring_req from mule
BLE ON -> scan/connect O2Ring -> read data -> BLE OFF
  |
  v
IDLE
```

The ezShare WiFi idle timeout (5 min) naturally disconnects WiFi between proxy bursts. For O2Ring requests, the miner explicitly disconnects WiFi before starting BLE, then re-enables WiFi on-demand for the next proxy request.

## BLE Protocol (Viatom)

Ported directly from cpapdash-push-c3 `o2ring_ble.c`. No changes to the BLE layer.

**Service UUID:** `14839ac4-7d7e-415c-9a42-167340cf2339`

| Characteristic | UUID | Role |
|----------------|------|------|
| Write | `8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3` | Send commands to ring |
| Notify | `0734594a-a8e7-4b1a-a6b1-cd5243059a57` | Receive responses from ring |

**Packet framing:** `[0xAA] [CMD] [CMD^0xFF] [BLOCK_LO] [BLOCK_HI] [LEN_LO] [LEN_HI] [PAYLOAD...] [CRC-8]`

**Commands:**

| CMD | Name | Payload | Response |
|-----|------|---------|----------|
| 0x14 | INFO | empty | JSON: `{"CurBAT":74,"Model":"O2Ring","SN":"...","FileList":"file1.vld,..."}` |
| 0x17 | READ_SENSORS | empty | Live SpO2/HR/motion/vibration (13+ bytes) |
| 0x03 | FILE_OPEN | filename + `\0` | File size as uint32_t LE |
| 0x04 | FILE_READ | empty (block in header) | Raw file data chunk |
| 0x05 | FILE_CLOSE | empty | Ack |

**Ring constraints:**
- Max 4 stored .vld sessions
- Standby mode required for file ops (ring off-wrist)
- BLE advertising stops after ~2 min in standby

## UART Protocol Extensions

New JSON message types over the existing UART link. Same newline-delimited JSON format.

### Mule -> Miner

**O2Ring status request:**
```json
{"type":"o2ring_req","id":10,"cmd":"status"}
```

**O2Ring file list request:**
```json
{"type":"o2ring_req","id":11,"cmd":"files"}
```

**O2Ring file download request:**
```json
{"type":"o2ring_req","id":12,"cmd":"download","name":"20260412065307.vld"}
```

**O2Ring live sensor read:**
```json
{"type":"o2ring_req","id":13,"cmd":"live"}
```

### Miner -> Mule

**O2Ring status response:**
```json
{"type":"o2ring_status","id":10,"connected":true,"model":"O2Ring","serial":"20243041276","battery":74,"file_count":2}
```

**O2Ring file list response:**
```json
{"type":"o2ring_files","id":11,"files":["20260412065307.vld","20260413231500.vld"],"battery":74}
```

**O2Ring live reading response:**
```json
{"type":"o2ring_live","id":13,"spo2":97,"hr":62,"motion":5,"vibration":0,"valid":true}
```

**O2Ring file download (reuses existing proxy_meta + proxy_chunk):**
```json
{"type":"proxy_meta","id":12,"st":200,"cl":34560,"ts":0}
{"type":"proxy_chunk","id":12,"seq":0,"d":"<base64>","last":false}
{"type":"proxy_chunk","id":12,"seq":1,"d":"<base64>","last":true}
```

File downloads reuse the existing `proxy_meta` + `proxy_chunk` framing — no new message types needed for the binary data path. The mule's file_server already knows how to stream these to HTTP clients.

**O2Ring error:**
```json
{"type":"error","id":10,"message":"O2Ring not found","code":"BLE_NOT_FOUND"}
```

Error codes: `BLE_NOT_FOUND`, `BLE_CONNECT_FAIL`, `BLE_TIMEOUT`, `BLE_READ_FAIL`, `WIFI_BUSY` (if WiFi can't be released).

## HTTP API (Mule)

New endpoints on the existing HTTP server (port 80). Each one sends an `o2ring_req` over UART and waits for the miner's response.

### `GET /o2ring/status`

Returns BLE connection state and cached device info.

```json
{"connected":true,"model":"O2Ring","serial":"20243041276","battery":74,"file_count":2}
```

If not connected: `{"connected":false}`

### `GET /o2ring/files`

Triggers BLE INFO command, returns fresh file list.

```json
{"files":["20260412065307.vld","20260413231500.vld"],"battery":74}
```

Returns 408 on BLE timeout, 502 on BLE error.

### `GET /o2ring/files?name=20260412065307.vld`

Downloads specified .vld file from ring via BLE. Streamed to HTTP client using existing chunked proxy mechanism.

- **Content-Type:** `application/octet-stream`
- **Content-Disposition:** `attachment; filename="20260412065307.vld"`
- **Typical size:** 30-50 KB per 8-hour session
- **Download time:** ~15-30 seconds (BLE transfer + UART relay)

Returns 408 on BLE timeout, 400 on invalid filename, 502 on BLE error.

### `GET /o2ring/live`

Triggers BLE READ_SENSORS, returns live oximetry reading.

```json
{"spo2":97,"hr":62,"motion":5,"vibration":0,"valid":true}
```

Ring must be on-wrist and recording for live data. Returns 408 on timeout.

### `GET /api/status` (updated)

Add O2Ring connection state to existing status endpoint:

```json
{"state":"proxy","wifi":true,"mqtt":false,"o2ring":true,"uptime":"01:23:45"}
```

## Miner Implementation

### Scanner Task Changes

Add new states to `scanner_task.c` state machine:

```
SCANNER_IDLE
  ↓ proxy_req
SCANNER_PROXY (existing ezShare flow)
  ↓
SCANNER_IDLE
  ↓ o2ring_req
SCANNER_O2RING              ← NEW
  ↓
SCANNER_IDLE
```

The `SCANNER_O2RING` state:

1. Disconnect ezShare WiFi (if connected)
2. Init BLE stack (if not already done)
3. Handle command:
   - `status`: return cached connection state
   - `files`: `o2ring_ble_connect_and_wait()` -> `o2ring_ble_refresh_info()` -> send file list
   - `download`: connect -> `o2ring_ble_download_file()` -> stream as proxy_meta + proxy_chunk
   - `live`: connect -> `o2ring_ble_read_sensors()` -> send live data
4. Disconnect BLE (optional — can stay connected for subsequent requests)
5. Return to SCANNER_IDLE

### BLE Lifecycle on Miner

**Lazy init:** BLE stack is initialized on the first `o2ring_req`. Not at boot — saves ~50KB heap when O2Ring is not used.

**Connection persistence:** After the first `o2ring_req`, the miner stays BLE-connected to the ring (auto-reconnect enabled). This avoids the 5-15s scan+connect overhead on every request. The connection drops naturally when the ring enters recording mode or goes out of range.

**WiFi/BLE handoff:** When an `o2ring_req` arrives while WiFi is connected:
1. Disconnect WiFi (`wifi_manager_disconnect()`)
2. Wait 100ms for radio release
3. Proceed with BLE operation
4. WiFi reconnects on-demand when the next `proxy_req` arrives

When a `proxy_req` arrives while BLE is connected:
1. Stop BLE scan if active
2. Disconnect BLE (`o2ring_ble_disconnect()`)
3. Deinit BLE stack (`o2ring_ble_deinit()`) to free heap
4. Connect WiFi and handle proxy request

### Memory Budget (Miner)

| Component | Heap (active) | Notes |
|-----------|--------------|-------|
| WiFi STA | ~30KB | Off during BLE |
| HTTP client | ~10KB | Off during BLE |
| BLE stack | ~50KB | Off during WiFi |
| UART handler | ~16KB | Always on |
| Scanner task | 8KB stack | Always on |
| **Free heap** | **~60KB** | Never WiFi+BLE concurrent |

Total DRAM: ~160KB on ESP32-C3. Budget is tight but viable since WiFi and BLE never overlap.

### Enable BLE in sdkconfig

Add to `miner/sdkconfig.defaults`:
```
CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_GATT_MAX_SR_PROFILES=2
CONFIG_BT_GATT_MAX_SR_ATTRIBUTES=30
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
```

### Port o2ring_ble.c

Copy from cpapdash-push-c3 `miner/main/o2ring_ble.c` and `o2ring_ble.h` with minimal changes:
- Remove any SPI-specific code (there is none — the BLE layer is self-contained)
- Keep all Viatom protocol handling, CRC-8, GATT callbacks, file download logic
- The streaming download (`o2ring_ble_download_file_stream`) is useful for UART chunking

## Mule Implementation

### file_server.c Changes

Add new HTTP handlers:

- `handle_o2ring_status()` — sends `o2ring_req` cmd=status, waits for `o2ring_status` response
- `handle_o2ring_files()` — if `name` param present: sends cmd=download, reuses existing proxy_meta/chunk streaming. Otherwise: sends cmd=files, waits for `o2ring_files` response
- `handle_o2ring_live()` — sends cmd=live, waits for `o2ring_live` response

All handlers use `s_proxy_mutex` — only one UART operation at a time (same as ezShare proxying).

Register new URI handlers:
- `/o2ring/status`
- `/o2ring/files`
- `/o2ring/live`

### O2Ring UART Timeout

O2Ring BLE operations take longer than ezShare HTTP:
- Status check: 1-2s (if already connected) or 15-20s (scan + connect)
- File list: 2-5s
- File download: 15-60s (depends on file size)
- Live reading: 2-5s

The mule should use a longer UART timeout for O2Ring requests: 30s for status/files/live, 120s for downloads. The existing `PROXY_REQ_TIMEOUT_MS` (60s) works for most cases but downloads may need extension.

## Files to Modify

**Miner (new):**
- `main/o2ring_ble.c` — port from cpapdash-push-c3
- `main/o2ring_ble.h` — port from cpapdash-push-c3

**Miner (modify):**
- `main/CMakeLists.txt` — add `bt` to REQUIRES, add `o2ring_ble.c` to SRCS
- `main/scanner_task.c` — add SCANNER_O2RING state, handle `o2ring_req` messages, WiFi/BLE handoff
- `main/scanner_task.h` — add SCANNER_O2RING to enum
- `main/config.h` — add BLE timeouts, log tag
- `sdkconfig.defaults` — enable BT

**Mule (modify):**
- `main/file_server.c` — add /o2ring/* HTTP handlers, O2Ring UART request/response handling
- `main/file_server.h` — no changes (handlers registered internally)
- `main/config.h` — add O2Ring timeout constants

## Testing

1. **BLE scan:** Flash miner with BLE enabled. Verify it finds "O2Ring" in scan results.
2. **Status endpoint:** `curl http://<MULE_IP>/o2ring/status` — should return connected:true/false.
3. **File list:** Put ring in standby. `curl http://<MULE_IP>/o2ring/files` — should return .vld filenames.
4. **File download:** `curl http://<MULE_IP>/o2ring/files?name=<filename>.vld -o test.vld` — verify file size and VLD header.
5. **Live reading:** Put ring on finger. `curl http://<MULE_IP>/o2ring/live` — should return SpO2/HR.
6. **WiFi/BLE handoff:** Alternate between `/dir?dir=A:` and `/o2ring/status` — verify both work without crashes.
7. **Heap monitoring:** Run for 10+ minutes cycling both WiFi and BLE operations. Verify no heap exhaustion.
8. **Idle timeout:** After O2Ring operations, verify ezShare proxy still works (WiFi reconnects on demand).
