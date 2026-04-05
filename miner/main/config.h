#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

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

// UART (miner TX=21, RX=20 — crossed with mule TX=20, RX=21)
#define UART_PORT_NUM               UART_NUM_1
#define UART_BAUD_RATE              115200
#define UART_TX_PIN                 GPIO_NUM_21
#define UART_RX_PIN                 GPIO_NUM_20
#define UART_RX_BUFFER_SIZE         8192
#define UART_TX_BUFFER_SIZE         8192
#define UART_QUEUE_SIZE             20

// Scanner task
#define SCANNER_TASK_STACK_SIZE     8192
#define SCANNER_TASK_PRIORITY       5
#define SCANNER_POLL_INTERVAL_MS    100
#define SCANNER_RETRY_DELAY_MS      10000

// Memory
#define MAX_FILES_PER_SCAN          10
#define MAX_FILE_SIZE               (2 * 1024 * 1024)
#define MAX_FILENAME_LEN            256
#define MAX_DATE_FOLDERS            10
#define JSON_BUFFER_SIZE            4096

// Log tags
#define LOG_TAG_SCANNER             "MINER"
#define LOG_TAG_WIFI                "WIFI"
#define LOG_TAG_EZSHARE             "EZSHARE"
#define LOG_TAG_UART                "UART"
