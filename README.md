# hms-mm

> **EXPERIMENTAL** -- This project is in early development. Contributions welcome.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue.svg?logo=espressif)](https://docs.espressif.com/projects/esp-idf/)
[![Status: Experimental](https://img.shields.io/badge/Status-Experimental-orange.svg)]()

Dual ESP32-C3 miner/mule system for bridging WiFi SD cards to your home network. The miner connects to the SD card's WiFi AP, downloads files, and sends them to the mule over UART. The mule connects to your home WiFi and serves the files over HTTP.

Solves the "two WiFi networks" problem: WiFi SD cards create their own AP, so a single-radio device can't be on both the SD card's network and your home network simultaneously. Two ESP32-C3s, connected by UART, each handle one network.

## Architecture

```
WiFi SD Card AP (192.168.4.1)
    |
    v  (WiFi)
+------------------+       UART       +------------------+
|  Miner (ESP32-C3)|  <------------>  |  Mule (ESP32-C3) |
|  Downloads files |  TX=21, RX=20    |  Serves over HTTP|
|  from SD card    |  JSON + newline  |  on home WiFi    |
+------------------+                  +------------------+
                                            |
                                            v  (WiFi)
                                      Home network
                                            |
                                            v
                                      Any HTTP client
```

## Setup

### 1. Install ESP-IDF

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/).

```bash
. ~/esp/esp-idf/export.sh
```

### 2. Build and Flash

```bash
# Flash miner
cd miner
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

# Flash mule (separate USB port)
cd ../mule
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB1 flash monitor
```

### 3. Wire UART

Connect miner and mule with 2 signal wires + ground (TX/RX crossed):

| Miner | Mule |
|-------|------|
| GPIO 21 (TX) | GPIO 21 (RX) |
| GPIO 20 (RX) | GPIO 20 (TX) |
| GND | GND |

### 4. Configure WiFi

On first boot with no stored credentials, the **mule** creates an open WiFi AP named `MM-Setup-XXXX`. Connect to it from your phone -- a setup page opens automatically (or navigate to `http://192.168.4.1`):

- **Home WiFi** -- select your network from the scan list and enter the password
- **SD Card WiFi** -- enter the SD card's WiFi SSID and password

On save:
1. Mule stores home WiFi credentials in its own NVS
2. Mule sends SD card WiFi credentials to miner via UART
3. Miner stores SD card credentials in its own NVS
4. Both devices reboot with the new credentials

Credentials persist across reboots. If `idf.py menuconfig` defaults are set in `config.h`, those are used as fallback when NVS is empty.

### 5. Test

```bash
# List directories
curl "http://<MULE_IP>/dir?dir=A:DATALOG"

# List files in a directory
curl "http://<MULE_IP>/dir?dir=A:DATALOG%5C20260329"

# Download a file
curl "http://<MULE_IP>/download?file=DATALOG%5C20260329%5Cfile.edf" -o file.edf

# Check status
curl "http://<MULE_IP>/api/status"
```

## Credential Priority

| Priority | Source | How to set |
|----------|--------|------------|
| 1 | NVS (runtime) | Captive portal setup form |
| 2 | config.h (compile-time) | Edit source and rebuild |

NVS credentials override compile-time defaults. To force the captive portal again, erase NVS with `idf.py erase-flash`.

## HTTP API

| Endpoint | Description |
|----------|-------------|
| `GET /dir?dir=A:path` | HTML directory listing |
| `GET /dir?dir=A:path%5Csubdir` | HTML file listing for a subfolder |
| `GET /download?file=path` | Raw file download |
| `GET /api/status` | JSON status (WiFi, cached files, uptime) |

## UART Protocol

Newline-delimited JSON at 115200 baud between miner and mule.

**Mule -> Miner:**
- `{"type":"get_latest_req","max_age_hours":24}` -- request recent files
- `{"type":"set_config","ez_ssid":"...","ez_pass":"..."}` -- update SD card WiFi credentials

**Miner -> Mule:**
- `{"type":"file_data","path":"...","size":N,"content_base64":"..."}` -- file transfer
- `{"type":"file_array_complete","count":N,"total_bytes":N}` -- transfer complete
- `{"type":"config_ack","status":"ok"}` -- credentials stored, rebooting
- `{"type":"error","message":"...","code":"..."}` -- error

## Project Structure

```
hms-mm/
  version.h               # Shared version (YYYY.MINOR.PATCH)
  miner/                   # ESP32-C3 #1 (connects to SD card WiFi)
    main/
      main.c               # Boot, init, status loop
      scanner_task.c/h     # State machine: download files, send via UART
      uart_handler.c/h     # UART JSON TX/RX
      wifi_manager.c/h     # WiFi STA (connects to SD card AP on demand)
      ezshare_client.c/h   # HTTP client for SD card file API
      nvs_config.c/h       # NVS storage for SD card WiFi credentials
      config.h              # Pin assignments, defaults
    partitions.csv          # 2MB app partition
  mule/                    # ESP32-C3 #2 (connects to home WiFi)
    main/
      main.c               # Boot flow: NVS -> config.h -> captive portal
      mule_task.c/h        # State machine: request files, receive, decode, cache
      uart_handler.c/h     # UART JSON TX/RX
      wifi_manager.c/h     # WiFi STA (connects to home network)
      captive_portal.c/h   # AP mode WiFi setup with DNS hijack
      nvs_config.c/h       # NVS storage for WiFi + SD card credentials
      file_cache.c/h       # In-memory file cache
      file_server.c/h      # HTTP endpoints
      config.h              # Pin assignments, defaults
    partitions.csv          # 2MB app partition
```

## License

MIT
