#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

// Firmware identity
#define FW_PROJECT          "hms-mm"
#define FW_VERSION          "2026.0.6"

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
#define UART_BAUD_RATE              115200
#define UART_TX_PIN                 GPIO_NUM_2
#define UART_RX_PIN                 GPIO_NUM_3
#define UART_RX_BUFFER_SIZE         8192
#define UART_TX_BUFFER_SIZE         8192
#define UART_QUEUE_SIZE             20

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
