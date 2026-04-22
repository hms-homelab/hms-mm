#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

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
#define UART_RX_BUFFER_SIZE         8192
#define UART_TX_BUFFER_SIZE         8192
#define UART_QUEUE_SIZE             20

// Mule task (boot-time config only)
#define MULE_TASK_STACK_SIZE        8192
#define MULE_TASK_PRIORITY          5
#define MULE_BOOT_DELAY_SEC         5

// Proxy configuration
#define PROXY_CHUNK_SIZE            4096
#define PROXY_UART_BUF_SIZE         8192
#define PROXY_REQ_TIMEOUT_MS        60000   // per-chunk UART receive timeout

// Captive portal
#define PORTAL_AP_CHANNEL           1
#define PORTAL_MAX_CONN             2

// Memory
#define JSON_BUFFER_SIZE            4096

// Log tags
#define LOG_TAG_MULE                "MULE"
#define LOG_TAG_WIFI                "WIFI"
#define LOG_TAG_UART                "UART"
