#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

// Firmware identity. The version lives in this board's own version.h — the mule
// and miner version independently. Do not define a version literal here.
#include "version.h"
#define FW_PROJECT          "hms-mm"
#define FW_VERSION          FIRMWARE_VERSION

// =============================================================================
// Miner config — ezShare creds from NVS first, Kconfig fallback
// =============================================================================

// ezShare WiFi defaults (overridden by NVS if captive portal was used)
#define EZSHARE_WIFI_SSID_DEFAULT   "ez Share"
#define EZSHARE_WIFI_PASSWORD_DEFAULT "88888888"
#define WIFI_MAXIMUM_RETRY          5
#define WIFI_CONNECT_TIMEOUT_MS     10000

// ezShare HTTP
#define EZSHARE_IP                  "192.168.4.1"
#define EZSHARE_PORT                80
#define EZSHARE_DIR_PATH            "/dir?dir=A:DATALOG"
#define HTTP_TIMEOUT_MS             30000
#define HTTP_BUFFER_SIZE            4096

// UART: TX=GPIO2, RX=GPIO3. Both boards use identical pins; the 3D-printed
// tape board does the TX->RX crossover via the 180-deg module layout.
// GPIO2 is a strapping pin but is always TX (idles high), so it stays boot-safe.
#define UART_PORT_NUM               UART_NUM_1
// Must match the mule exactly — see the note in mule/main/config.h. Changing
// the baud is a breaking link change: flash both boards together.
#define UART_BAUD_RATE              921600
#define UART_TX_PIN                 GPIO_NUM_2
#define UART_RX_PIN                 GPIO_NUM_3
#define UART_RX_BUFFER_SIZE         16384
#define UART_TX_BUFFER_SIZE         8192
#define UART_QUEUE_SIZE             20
// Longest single newline-delimited frame; see mule/main/config.h.
#define UART_LINE_MAX               8192

// Scanner task
#define SCANNER_TASK_STACK_SIZE     8192
#define SCANNER_TASK_PRIORITY       5
#define SCANNER_POLL_INTERVAL_MS    100
#define SCANNER_RETRY_DELAY_MS      10000

// Streaming chunk configuration
#define FILE_CHUNK_SIZE             4096
#define PROXY_UART_BUF_SIZE         8192
#define PROXY_IDLE_TIMEOUT_MS       300000  // 5 min — disconnect ezShare after idle

// Memory
#define MAX_FILES_PER_SCAN          10
#define MAX_FILENAME_LEN            256
#define MAX_DATE_FOLDERS            10
#define JSON_BUFFER_SIZE            4096

// OTA rollback watchdog: how long a freshly written, still-unconfirmed image
// has to decode a frame from the mule before it reboots and lets the
// bootloader revert. Generous, because the mule's own boot can legitimately be
// slow, and the cost of being wrong in this direction is a needless rollback
// while the cost in the other direction is a miner nobody can reach.
#define MINER_OTA_CONFIRM_TIMEOUT_MS 180000

// O2Ring BLE
#define O2RING_CONNECT_TIMEOUT_MS   15000
#define O2RING_CMD_TIMEOUT_MS       10000
#define O2RING_DOWNLOAD_TIMEOUT_MS  120000

// Log tags
#define LOG_TAG_SCANNER             "MINER"
#define LOG_TAG_WIFI                "WIFI"
#define LOG_TAG_EZSHARE             "EZSHARE"
#define LOG_TAG_UART                "UART"
#define LOG_TAG_O2RING              "O2RING"
