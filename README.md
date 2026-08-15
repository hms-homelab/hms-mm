# hms-mm

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue.svg?logo=espressif)](https://docs.espressif.com/projects/esp-idf/)

Dual ESP32-C3 proxy bridge for WiFi SD cards and Wellue O2Ring oximeters. The mule connects to your home WiFi and runs an HTTP server. When a client requests a file, the mule forwards the request to the miner over UART. The miner connects to the SD card's WiFi AP or O2Ring via BLE, fetches the data, and streams it back chunk by chunk.

Solves the "two WiFi networks" problem: WiFi SD cards create their own AP, so a single-radio device can't be on both the SD card's network and your home network simultaneously. Two ESP32-C3s, connected by UART, each handle one network. The miner also supports BLE connections to a Wellue O2Ring for oximetry data (SpO2, heart rate, stored session files).

## The device page

Point a browser at the mule for status, the SD card, firmware updates, recent
logs and the maintenance actions. No app, no account, no cloud.

<img src="docs/img/device-page.png" alt="hms-mm device page" width="380">

## Architecture

```
                         UART (921600 baud)
WiFi SD Card AP          JSON + newline          Home Network
(192.168.4.1)            TX/RX crossed           (your router)
    |                                                 |
    v  WiFi                                     WiFi  v
+------------------+   GPIO 2/3 UART  +------------------+
|  Miner (ESP32-C3)|  <------------>  |  Mule (ESP32-C3) |
|                  |                  |                  |
|  Connects to SD  |  proxy_req -->   |  HTTP server :80 |
|  card WiFi on    |  <-- proxy_meta  |  Forwards /dir & |
|  demand, streams |  <-- proxy_chunk |  /download to    |
|  chunks back     |  chunk_ack -->   |  miner via UART  |
|                  |  o2ring_req -->  |                  |
|  Connects to     |  <-- o2ring_*   |  /o2ring/* API   |
|  O2Ring via BLE  |                  |                  |
+------------------+                  +------------------+
       ^                                    |
       |  BLE                               v  HTTP
  O2 Ring                             Browser / App
```

**Request flow:**

1. Client sends `GET /dir?dir=A:DATALOG` to the mule
2. Mule takes the link lock and sends `proxy_req` over UART
3. Miner connects to the ezShare WiFi on demand and fetches from the card
4. Miner sends `proxy_meta` (status, length), then `proxy_chunk` frames
5. Mule verifies each chunk, streams it to the HTTP client, then replies
   `chunk_ack` — which is what releases the miner to send the next one
6. Mule refuses to finalise the response unless the byte count matches the
   length the card promised
7. Miner disconnects from the ezShare WiFi after an idle timeout (5 min)

### Why every chunk is acknowledged

The link is two wires. There is no hardware flow control, so if the miner
streams at wire speed while the mule is still decoding the previous chunk and
pushing it out over WiFi, the surplus is simply lost. Chunk CRCs catch that but
cannot repair it. Acknowledging each chunk paces the miner to what the mule
actually absorbs.

On one bench setup, free-running transfers lost roughly a third of their
downloads to CRC mismatches, and lowering the baud did not reliably help, while
the same transfers acknowledged completed essentially every time. Those runs
are in the commit history rather than here, deliberately: they came from one
pair of boards joined one particular way, and how you join yours is your
choice — jumper wires, copper tape, a board you had made. What one setup
carries cleanly another will not.

So take the mechanism, not the number. Acknowledging paces the miner to what
the mule can absorb while it is busy on WiFi, and that holds however the boards
are wired. `UART_BAUD_RATE` is the one knob, it must match on both boards, and
if `/api/logs` shows chunk CRC mismatches, lower it.

## Setup

### 1. Install ESP-IDF

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/).

```bash
. ~/esp/esp-idf/export.sh
```

### 2. Wire UART

Connect miner and mule with 2 signal wires + ground. Both boards use the same
pins (TX = GPIO2, RX = GPIO3); cross them so each board's TX reaches the other's RX:

| Miner | Mule | Signal |
|-------|------|--------|
| GPIO 3 (RX) | GPIO 2 (TX) | Mule TX -> Miner RX |
| GPIO 2 (TX) | GPIO 3 (RX) | Miner TX -> Mule RX |
| GND | GND | Common ground |
| 3V3 | 3V3 | Shared power rail (power either board) |

Tie the two 3V3 pins together and you can power the whole bridge from a single
USB-C on either board. GPIO2 is a strapping pin, but it is always the TX line
(which idles high), so it stays boot-safe. Prefer the
[3D-printed board](hardware/3d-pcb/) below, which does the TX/RX crossover and the
3V3/GND rails for you in copper.

> **If you have built this before, delete `sdkconfig` first.** ESP-IDF only reads
> `sdkconfig.defaults` when `sdkconfig` does not already exist, so an existing
> build directory silently ignores new defaults. That is not cosmetic here: it
> is how OTA rollback protection came to be switched off on a board whose
> firmware reported it as enabled. Verify after building:
>
> ```bash
> grep CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE mule/sdkconfig miner/sdkconfig
> ```

### 3. Build and Flash

```bash
# Flash miner
cd miner
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash

# Flash mule (separate USB port)
cd ../mule
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodemYYYY flash
```

### 4. Configure WiFi (Captive Portal)

<img src="docs/img/setup-portal.png" alt="hms-mm setup portal" width="300">


On first boot (or after `idf.py erase-flash`), the mule creates an open WiFi AP:

**AP name:** `MM-Setup-XXXX` (last 4 chars of device serial)

1. Connect to the AP from your phone or laptop
2. A setup page opens automatically (or go to `http://192.168.4.1`)
3. Enter your **home WiFi** credentials (SSID + password)
4. Enter the **SD card WiFi** credentials (e.g. `ez Share` / `88888888` for ezShare cards)
5. Save

On save:
- Mule stores home WiFi credentials in NVS
- Mule sends SD card WiFi credentials to miner over UART
- Miner stores SD card credentials in NVS
- Both devices reboot with the new credentials

Credentials persist across reboots. To re-enter setup, use **Reset Wi-Fi** on the
device page (or `POST /api/reset`), which keeps the card settings. `idf.py
erase-flash` also works and forgets everything.

### 5. Test

```bash
# Check mule is up
curl "http://<MULE_IP>/api/status"
# {"serial":"MM-4F2A","fw":"2026.0.6","miner_fw":"2026.0.6","wifi":true,...}

# List root directory
curl "http://<MULE_IP>/dir?dir=A:"

# List DATALOG subdirectory
curl "http://<MULE_IP>/dir?dir=A:DATALOG"

# List files in a date folder
curl "http://<MULE_IP>/dir?dir=A:DATALOG%5C20260329"

# Download a file
curl "http://<MULE_IP>/download?file=DATALOG%5C20260329%5Cfile.edf" -o file.edf

# Download with Range (partial content)
curl -H "Range: bytes=1024-" "http://<MULE_IP>/download?file=STR.EDF" -o str_partial.edf
```

## HTTP API

Everything is served from one HTTP server on port 80. Point a browser at the
mule's address for a device page with status, the SD card links, the O2 Ring
switch, recent logs and the maintenance actions.

### Device and control

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Device page |
| `/api/status` | GET | JSON device status (below) |
| `/api/logs?n=120` | GET | Recent log lines as plain text. `n` tails the last N lines. |
| `/api/reboot` | POST | Restart. Body `{"target":"mule"\|"miner"\|"both"}`, default `mule`. Config is kept. |
| `/api/reset` | POST | Forget the home WiFi and return to the setup portal. Resets the miner too. The ezShare credentials and the serial are kept. |
| `/api/config` | POST | `{"wifi_ssid","wifi_pass","ez_ssid","ez_pass"}` to write credentials, or `{"o2_enabled":bool}` on its own to toggle the O2 Ring without touching anything else. |

### Firmware update

<img src="docs/img/firmware-page.png" alt="hms-mm firmware page" width="300">

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/update` | GET | Update page |
| `/api/ota` | POST | `{"target":"mule"\|"miner","url":"..."}`. The device fetches and installs it, so the address must be reachable **from the device**. Returns `202`; follow progress in `/api/status`. |
| `/api/update/mule` · `/api/update/miner` | POST | Raw `.bin` in the body. Needs no network at all. |
| `/api/cancel_update` | POST | Abandon a URL pull in progress |

Nothing polls for updates. The device contacts a server only when you hand it a
URL. `https://` is verified against the bundled root certificates, because this
installs executable code; `http://` from a NAS or a laptop skips TLS and is the
lighter path on a LAN.

While an update runs, every route that drives the miner or changes
configuration answers `503` with `Retry-After`. `/api/status`, `/api/logs` and
`/api/cancel_update` stay available, since those are how you watch an update
and how you stop one.

### ezShare SD card

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/dir?dir=A:path` | GET | HTML directory listing (proxied from the card) |
| `/download?file=path` | GET | Raw file download, supports `Range` header |

**`/api/status` response:**
```json
{"serial":"MM-4F2A","fw":"2026.0.6","miner_fw":"2026.0.6","state":"proxy",
 "wifi":true,"uptime":"01:23:45","rssi":-58,"free_heap":54321,
 "largest_block":18000,"min_free":41000,"o2_enabled":false,
 "ezshare":{"assoc":false,"rssi":0,"reason":201,"age_s":42}}
```

`fw` and `miner_fw` can legitimately differ: the two boards version
independently. `largest_block` is the largest contiguous free block and matters
more than `free_heap` on a C3, where an allocation fails from fragmentation
long before memory actually runs out. The `ezshare` object is the miner's view
of the card from its last reported error, and is absent until something has
failed, so it answers "why did that request fail" rather than acting as a live
probe.

**`/download` Range support:**
```
GET /download?file=STR.EDF HTTP/1.1
Range: bytes=1024-2047

HTTP/1.1 206 Partial Content
Content-Range: bytes 1024-2047/75264
Accept-Ranges: bytes
```

### O2Ring Oximetry (BLE)

The miner connects to a [Wellue O2Ring](https://www.wellue.com/o2ring) via BLE. The ring must be in standby mode (off-wrist) for file operations. Live readings require the ring to be on-finger and recording.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/o2ring/status` | GET | BLE connection state + device info |
| `/o2ring/files` | GET | List stored .vld session files on the ring |
| `/o2ring/files?name=FILE.vld` | GET | Download a .vld file from the ring |
| `/o2ring/live` | GET | Live SpO2/HR reading (ring must be on-finger) |

**`/o2ring/status` response:**
```json
{"connected":true,"model":"O2Ring","serial":"20243041276","battery":74,"file_count":2}
```

**`/o2ring/files` response:**
```json
{"files":["20260412065307.vld","20260413231500.vld"],"battery":74}
```

**`/o2ring/live` response:**
```json
{"spo2":97,"hr":62,"motion":5,"vibration":0,"valid":true}
```

**`/o2ring/files?name=...` response:** Binary `.vld` file download (`application/octet-stream`). Typical size: 30-50 KB per 8-hour session. Download time: ~15-30 seconds over BLE.

WiFi and BLE share the ESP32-C3 radio, so they run sequentially — the miner disconnects from ezShare WiFi before starting BLE operations, and reconnects on demand for the next SD card request.

## UART Protocol

Newline-delimited JSON at 460800 baud — one frame per line, so the link stays
readable in a serial monitor. Both boards must run the same baud, so a change
here means reflashing the pair. The rate is measured rather than chosen; see
[Why every chunk is acknowledged](#why-every-chunk-is-acknowledged).

### Mule -> Miner

**Proxy request:**
```json
{"type":"proxy_req","id":1,"path":"/dir?dir=A:DATALOG","rs":0,"re":0}
```
- `id` — request ID for matching responses
- `path` — ezShare URL path
- `rs`/`re` — Range start/end (0 = no range)

**Set config (at boot):**
```json
{"type":"set_config","ez_ssid":"ez Share","ez_pass":"88888888"}
```

### Miner -> Mule

**Proxy metadata (sent before first chunk):**
```json
{"type":"proxy_meta","id":1,"st":200,"cl":1528,"ts":0}
```
- `st` — HTTP status from ezShare (200 or 206)
- `cl` — content length
- `ts` — total file size (for Range responses)

**Proxy chunk (streamed, base64-encoded):**
```json
{"type":"proxy_chunk","id":1,"seq":0,"c":2276608578,"d":"<base64>","last":false}
```
- `seq` — chunk sequence number. A gap aborts the transfer; the mule will not
  hand the client a response with a hole in it.
- `c` — CRC32 of the **decoded** bytes. The link has no parity, checksum or
  hardware flow control, and base64 will happily decode a frame that lost bytes
  into shorter, well-formed garbage — so a successful decode proves nothing and
  this is what actually catches corruption. Sent as a JSON number; read it as a
  double, since a CRC above 2^31 will not fit a 32-bit signed int.
- `d` — base64-encoded binary data
- `last` — true on final chunk

**Chunk acknowledgement (mule -> miner):**
```json
{"type":"chunk_ack","id":1,"seq":0}
```
Sent only once the decoded bytes have been handed to the HTTP client. The miner
blocks after every non-final chunk until this arrives, which is the link's only
flow control. No ack is sent for the final chunk: the mule finalises the
response rather than asking for more. A miner that waits
`PROXY_CHUNK_ACK_TIMEOUT_MS` (10 s) without hearing one gives up on the
transfer. `proxy_abort` arriving instead of an ack means the client went away.

**Error:**
```json
{"type":"error","id":1,"message":"ezShare unreachable","code":"WIFI_FAILED",
 "assoc":false,"rssi":0,"reason":201}
```
Every error carries the miner's view of the ezShare link, because otherwise all
failures look like an identical `502` from the mule's side:

| Diagnostic | Meaning |
|---|---|
| `assoc:false, reason:201` | AP not found — card off, asleep, or out of range |
| `assoc:false, reason:202` | Authentication failed — wrong card password |
| `assoc:true, rssi<=-80` | Associated but too weak to move data |

The mule keeps the latest of these and reports it under `ezshare` in
`/api/status`.

**Config acknowledgment:**
```json
{"type":"config_ack","status":"ok","changed":true}
```
`changed` tells the mule whether the miner is about to restart to apply new
credentials. Identical credentials are acknowledged without a restart, so a
mule reboot no longer drags the miner down with it.

### Control messages

Small request/response exchanges, all mule -> miner unless noted.

| Send | Reply | Purpose |
|---|---|---|
| `{"type":"version_req"}` | `{"type":"version_resp","ver":"2026.0.6"}` | Miner firmware version; surfaces as `miner_fw` in `/api/status` |
| `{"type":"reboot"}` | `{"type":"ack","of":"reboot"}` | Restart the miner, **config intact** |
| `{"type":"reset"}` | `{"type":"ack","of":"reset"}` | Erase miner config and restart; the mule re-pushes credentials on its next boot |
| `{"type":"o2_state_req"}` | `{"type":"o2_state_resp","enabled":false}` | Read the O2Ring BLE gate |
| `{"type":"o2_set_enabled","enabled":true}` | `{"type":"o2_state_resp","enabled":true}` | Set the gate. The miner restarts to apply it, since the BLE stack is only brought up at init. A no-op change does not restart. |

**Firmware update** (mule -> miner), which the mule drives from either an HTTP
download or a body upload:

```json
{"type":"ota_begin","total":1714912}
{"type":"ota_chunk","off":0,"c":2276608578,"d":"<base64>"}
{"type":"ota_finish"}
```
Each frame is acknowledged before the next is sent. `off` is checked against
the miner's own byte count, so a dropped or duplicated chunk fails the transfer
instead of quietly writing a misaligned image, and `c` is verified before the
bytes reach flash. While a transfer is open the miner accepts only `ota_chunk`,
`ota_finish` and `ota_abort`, and refuses everything else with `OTA_BUSY`.

An `ack` may be lost — the miner can restart before it drains — so a missing
ack is not proof the command was ignored.

### O2Ring Messages

**O2Ring request (mule -> miner):**
```json
{"type":"o2ring_req","id":10,"cmd":"status"}
{"type":"o2ring_req","id":11,"cmd":"files"}
{"type":"o2ring_req","id":12,"cmd":"download","name":"20260412065307.vld"}
{"type":"o2ring_req","id":13,"cmd":"live"}
```
- `cmd` — `status`, `files`, `live`, or `download`
- `name` — filename for download command

**O2Ring responses (miner -> mule):**
```json
{"type":"o2ring_status","id":10,"connected":true,"model":"O2Ring","serial":"...","battery":74,"file_count":2}
{"type":"o2ring_files","id":11,"files":["20260412065307.vld"],"battery":74}
{"type":"o2ring_live","id":13,"spo2":97,"hr":62,"motion":5,"vibration":0,"valid":true}
```

File downloads reuse `proxy_meta` + `proxy_chunk` framing (same as ezShare).

## Credential Priority

| Priority | Source | How to set |
|----------|--------|------------|
| 1 | NVS (runtime) | Captive portal setup form |
| 2 | config.h (compile-time) | Edit source and rebuild |

NVS credentials override compile-time defaults.

## Project Structure

```
hms-mm/
  miner/                    # ESP32-C3 #1 (connects to SD card WiFi + O2Ring BLE)
    main/
      main.c                # Boot, init, status loop
      scanner_task.c/h      # UART handler: proxy_req, o2ring_req, set_config
      uart_handler.c/h      # UART JSON TX/RX
      wifi_manager.c/h      # WiFi STA (connects to SD card AP on demand)
      ezshare_client.c/h    # HTTP client for SD card with chunked streaming callback
      o2ring_ble.c/h        # Wellue O2Ring BLE GATT client (Viatom protocol)
      nvs_config.c/h        # NVS storage for SD card WiFi credentials
      config.h              # Pin assignments, timeouts, defaults
    partitions.csv
  mule/                     # ESP32-C3 #2 (connects to home WiFi)
    main/
      main.c                # Boot flow: NVS -> config.h -> captive portal
      mule_task.c/h         # Sends ezShare config to miner at boot
      uart_handler.c/h      # UART JSON TX/RX
      wifi_manager.c/h      # WiFi STA (connects to home network)
      captive_portal.c/h    # AP mode WiFi setup with DNS hijack
      file_server.c/h       # HTTP proxy server + O2Ring endpoints
      nvs_config.c/h        # NVS storage for WiFi + SD card credentials
      config.h              # Pin assignments, timeouts, defaults
    partitions.csv
```

## 3D-Printed Board

There is a 3D-printed "tape PCB" for this project in [`hardware/3d-pcb/`](hardware/3d-pcb/):
a single-layer printed substrate where copper foil tape laid into grooves becomes the
traces, no fab. Two ESP32-C3 SuperMini pockets sized from the real footprint, the two UART
signals routed as parallel non-crossing diagonals across the gap, GND and a shared 3V3 rail
looped back through the interior, and both SuperMinis flush to the edges so each USB-C
overhangs and a cable plugs straight in. Power the whole bridge from either USB-C.

- `hms-mm-uart-tape-board.scad` (OpenSCAD source; set `part` to "board", "lid", or "both")
- `hms-mm-uart-tape-board.stl` (the substrate, ready to print)
- `hms-mm-uart-tape-lid.stl` (snap-on lid with a USB-C window per port)

Print it in something tougher than PLA if you want to sand the copper back flush. The full
story of the technique, and how this whole thing started as an ugly copper-tape prototype,
is written up here: [My very first ugly working prototype (with a 3D printed PCB)](https://www.cpapdash.com/blog/ugly-prototype-3d-printed-pcb).

## License

MIT
