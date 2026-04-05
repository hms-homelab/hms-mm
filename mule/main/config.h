#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

// =============================================================================
// Mule config — home WiFi from NVS first, Kconfig fallback
// =============================================================================

// Home WiFi defaults (overridden by NVS if captive portal was used)
#define HOME_WIFI_SSID_DEFAULT      "your_wifi_ssid"
#define HOME_WIFI_PASSWORD_DEFAULT  "your_wifi_password"
#define WIFI_MAXIMUM_RETRY          5
#define WIFI_CONNECT_TIMEOUT_MS     15000

// UART (mule TX=20, RX=21 — crossed with miner TX=21, RX=20)
#define UART_PORT_NUM               UART_NUM_1
#define UART_BAUD_RATE              115200
#define UART_TX_PIN                 GPIO_NUM_20
#define UART_RX_PIN                 GPIO_NUM_21
#define UART_RX_BUFFER_SIZE         65536   // 64 KB for large file transfers
#define UART_TX_BUFFER_SIZE         8192
#define UART_QUEUE_SIZE             20

// Mule task
#define MULE_TASK_STACK_SIZE        12288
#define MULE_TASK_PRIORITY          5
#define MULE_POLL_INTERVAL_MS       100
#define MULE_COLLECTION_INTERVAL_SEC 300    // Request files every 5 minutes
#define MULE_BOOT_DELAY_SEC         30      // Wait after boot before first collection
#define MULE_MAX_AGE_HOURS          24

// File cache
#define FILE_CACHE_MAX_FILES        64
#define FILE_CACHE_MAX_TOTAL_BYTES  (4 * 1024 * 1024)  // 4 MB

// Captive portal
#define PORTAL_AP_CHANNEL           1
#define PORTAL_MAX_CONN             2

// Memory
#define JSON_BUFFER_SIZE            4096

// Log tags
#define LOG_TAG_MULE                "MULE"
#define LOG_TAG_WIFI                "WIFI"
#define LOG_TAG_UART                "UART"
