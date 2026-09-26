#pragma once

#include "sdkconfig.h"   /* CONFIG_HMS_MM_BOARD_* */
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
// Board (Kconfig "hms-mm board", chosen by the build target).
//
// WIFI_TX_POWER_QDBM: max radio transmit power, in quarter-dBm. On the C3
// SuperMini, 44 (11 dBm) is NOT a power saving: its PCB antenna distorts at
// the ~20 dBm default, so turning it down makes the device INTELLIGIBLE, not
// quieter. It is a workaround for that one antenna. On any other board it just
// yields a link too weak for unicast rates (broadcast still works, so the
// symptom is a device that joins, gets DHCP and answers mDNS but never a ping),
// so other boards run the radio default.
//
// UART: C3 SuperMini TX=GPIO2, RX=GPIO3 on both boards; the 3D-printed tape
// board does the TX->RX crossover via the 180-deg module layout. GPIO2 is a
// strapping pin but is always TX (idles high), so it stays boot-safe.
// AtomS3: miner TX=GPIO1, RX=GPIO2; the mule is the other way round.
#if defined(CONFIG_HMS_MM_BOARD_ATOM_S3)
#define WIFI_TX_POWER_QDBM          80      // 20 dBm, the radio default
#define UART_TX_PIN                 GPIO_NUM_1
#define UART_RX_PIN                 GPIO_NUM_2
#else   // CONFIG_HMS_MM_BOARD_C3_SUPERMINI
#define WIFI_TX_POWER_QDBM          44
#define UART_TX_PIN                 GPIO_NUM_2
#define UART_RX_PIN                 GPIO_NUM_3
#endif

// The ezShare link is started at boot and held; joins and drops are retried in
// the background on ez_retry.h's schedule, forever. No request waits for it.

// ezShare HTTP. 15 s per socket operation, and fail fast: the card's link is
// flaky, and a patient socket turns a trickling or blackholed connection into
// a multi-minute wedge. A failed chunk is retried by the client with a Range.
#define EZSHARE_IP                  "192.168.4.1"
#define EZSHARE_PORT                80
#define EZSHARE_DIR_PATH            "/dir?dir=A:DATALOG"
#define HTTP_TIMEOUT_MS             15000
#define HTTP_BUFFER_SIZE            4096
// A /dir request some cards answer with their index page instead of a
// listing, now and then. Retried this many times, this far apart.
#define EZSHARE_LIST_RETRIES        3
#define EZSHARE_LIST_RETRY_MS       400

#define UART_PORT_NUM               UART_NUM_1
// Must match the mule exactly — see the note in mule/main/config.h. Changing
// the baud is a breaking link change: flash both boards together.
#define UART_BAUD_RATE              460800
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
// How long the miner waits for the mule to acknowledge a chunk before giving
// up on the transfer. Generous: the mule may be blocked pushing the previous
// chunk to a slow HTTP client, which is exactly the condition this ack exists
// to absorb.
#define PROXY_CHUNK_ACK_TIMEOUT_MS  10000

// Memory
#define MAX_FILES_PER_SCAN          10
#define MAX_FILENAME_LEN            256
#define MAX_DATE_FOLDERS            10
#define JSON_BUFFER_SIZE            4096

// Crash-loop self-heal. Six consecutive crash-boots is well past "unlucky" and
// into "this will not fix itself"; a device that has stayed up for a minute
// has cleared every boot-time fault worth counting.
#define CRASH_LOOP_THRESHOLD        6
#define CRASH_GUARD_HEALTHY_SEC     60

// OTA rollback watchdog: how long a freshly written, still-unconfirmed image
// has to decode a frame from the mule before it reboots and lets the
// bootloader revert. Generous, because the mule's own boot can legitimately be
// slow, and the cost of being wrong in this direction is a needless rollback
// while the cost in the other direction is a miner nobody can reach.
#define MINER_OTA_CONFIRM_TIMEOUT_MS 180000

// O2Ring BLE. The ring link is held between requests too, so a live read
// after the first costs no reconnect and no service discovery. It is never
// held at the card's expense: the stack is dropped before every card
// transfer (the card's own burst needs nearly the whole heap), when the
// ezShare link drops (the reassociation gets the whole radio and heap), and
// after O2RING_LINK_IDLE_MS with no ring request.
#define O2RING_CONNECT_TIMEOUT_MS   15000
#define O2RING_CMD_TIMEOUT_MS       10000
#define O2RING_DOWNLOAD_TIMEOUT_MS  120000
#define O2RING_LINK_IDLE_MS         180000
// After a connect timeout, no new BLE attempt for this long, so a ring that is
// off or away does not have the stack brought up and torn down every request.
#define O2RING_BACKOFF_MS           30000

// Hang guard: the scanner task is on the task watchdog, fed at the top of its
// loop and for every card chunk it streams. Every wait inside it is bounded
// below this; the longest is a ring download, whose blocks arrive on the BLE
// host task while the scanner waits up to O2RING_DOWNLOAD_TIMEOUT_MS for the
// end. A scanner silent this long is stuck, and the watchdog restarts the
// board (crash_guard counts it like any other crash).
#define SCANNER_HANG_TIMEOUT_S      150

// Log tags
#define LOG_TAG_SCANNER             "MINER"
#define LOG_TAG_WIFI                "WIFI"
#define LOG_TAG_EZSHARE             "EZSHARE"
#define LOG_TAG_UART                "UART"
#define LOG_TAG_O2RING              "O2RING"
