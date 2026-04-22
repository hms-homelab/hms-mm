# hms-mm

> **EXPERIMENTAL** — This project is in early development (v0.2).

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue.svg?logo=espressif)](https://docs.espressif.com/projects/esp-idf/)
[![Status: Experimental](https://img.shields.io/badge/Status-Experimental-orange.svg)]()

Dual ESP32-C3 proxy bridge for WiFi SD cards. The mule connects to your home WiFi and runs an HTTP server. When a client requests a file, the mule forwards the request to the miner over UART. The miner connects to the SD card's WiFi AP, fetches the data, and streams it back chunk by chunk. No caching, no buffering — real-time proxy.

Solves the "two WiFi networks" problem: WiFi SD cards create their own AP, so a single-radio device can't be on both the SD card's network and your home network simultaneously. Two ESP32-C3s, connected by UART, each handle one network.

## Architecture

```
                         UART (115200 baud)
WiFi SD Card AP          JSON + newline          Home Network
(192.168.4.1)            TX/RX crossed           (your router)
    |                                                 |
    v  WiFi                                     WiFi  v
+------------------+    GPIO 20/21    +------------------+
|  Miner (ESP32-C3)|  <------------>  |  Mule (ESP32-C3) |
|                  |                  |                  |
|  Connects to SD  |  proxy_req -->   |  HTTP server :80 |
|  card WiFi on    |  <-- proxy_meta  |  Forwards /dir & |
|  demand, streams |  <-- proxy_chunk |  /download to    |
|  chunks back     |                  |  miner via UART  |
+------------------+                  +------------------+
                                            |
                                            v  HTTP
                                      Browser / App
```

**Request flow:**

1. Client sends `GET /dir?dir=A:DATALOG` to the mule
2. Mule acquires proxy mutex, sends `proxy_req` JSON over UART
3. Miner connects to ezShare WiFi (on-demand), fetches from SD card
4. Miner streams `proxy_meta` (HTTP status) + `proxy_chunk` (base64-encoded data) back
5. Mule decodes chunks and streams them to the HTTP client
6. Miner disconnects from ezShare WiFi after idle timeout (5 min)

## Setup

### 1. Install ESP-IDF

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/).

```bash
. ~/esp/esp-idf/export.sh
```

### 2. Wire UART

Connect miner and mule with 2 signal wires + ground (TX/RX crossed):

| Miner | Mule | Signal |
|-------|------|--------|
| GPIO 21 (TX) | GPIO 21 (RX) | Miner TX -> Mule RX |
| GPIO 20 (RX) | GPIO 20 (TX) | Mule TX -> Miner RX |
| GND | GND | Common ground |

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

Credentials persist across reboots. To re-enter setup, erase flash: `idf.py erase-flash`.

### 5. Test

```bash
# Check mule is up
curl "http://<MULE_IP>/api/status"
# {"state":"proxy","wifi":true,"mqtt":false,"uptime":"00:05:23"}

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

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Redirects to `/dir?dir=A:` |
| `/dir?dir=A:path` | GET | HTML directory listing (proxied from SD card) |
| `/download?file=path` | GET | Raw file download, supports `Range` header |
| `/api/status` | GET | JSON device status |
| `/api/reset` | POST | Clear WiFi credentials and reboot into captive portal |

**`/api/status` response:**
```json
{"state":"proxy","wifi":true,"mqtt":false,"uptime":"01:23:45"}
```

**`/download` Range support:**
```
GET /download?file=STR.EDF HTTP/1.1
Range: bytes=1024-2047

HTTP/1.1 206 Partial Content
Content-Range: bytes 1024-2047/75264
Accept-Ranges: bytes
```

## UART Protocol

Newline-delimited JSON at 115200 baud.

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
{"type":"proxy_chunk","id":1,"seq":0,"d":"<base64>","last":false}
```
- `seq` — chunk sequence number
- `d` — base64-encoded binary data
- `last` — true on final chunk

**Error:**
```json
{"type":"error","id":1,"message":"ezShare unreachable","code":"WIFI_FAILED"}
```

**Config acknowledgment:**
```json
{"type":"config_ack","status":"ok"}
```

## Credential Priority

| Priority | Source | How to set |
|----------|--------|------------|
| 1 | NVS (runtime) | Captive portal setup form |
| 2 | config.h (compile-time) | Edit source and rebuild |

NVS credentials override compile-time defaults.

## Project Structure

```
hms-mm/
  miner/                    # ESP32-C3 #1 (connects to SD card WiFi)
    main/
      main.c                # Boot, init, status loop
      scanner_task.c/h      # UART proxy handler: receive req, fetch, stream chunks
      uart_handler.c/h      # UART JSON TX/RX
      wifi_manager.c/h      # WiFi STA (connects to SD card AP on demand)
      ezshare_client.c/h    # HTTP client for SD card with chunked streaming callback
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
      file_server.c/h       # HTTP proxy server with mutex + Range support
      nvs_config.c/h        # NVS storage for WiFi + SD card credentials
      config.h              # Pin assignments, timeouts, defaults
    partitions.csv
```

## License

MIT
