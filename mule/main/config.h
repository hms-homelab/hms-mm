#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

// Firmware identity. The version lives in this board's own version.h — the mule
// and miner version independently. Do not define a version literal here.
#include "version.h"
#define FW_PROJECT          "hms-mm"
#define FW_VERSION          FIRMWARE_VERSION

// Home WiFi defaults (overridden by NVS if captive portal was used)
#define HOME_WIFI_SSID_DEFAULT      "your_wifi_ssid"
#define HOME_WIFI_PASSWORD_DEFAULT  "your_wifi_password"
// Max radio transmit power, in quarter-dBm. 44 = 11 dBm. NOT a power saving:
// the C3 SuperMini PCB antenna distorts at the ~20 dBm default, so turning it
// down makes the device INTELLIGIBLE, not quieter.
#define WIFI_TX_POWER_QDBM          44

#define WIFI_MAXIMUM_RETRY          5
#define WIFI_CONNECT_TIMEOUT_MS     15000
// Reconnect ladder, used only AFTER a first successful join. A drop then means
// the network exists and the credentials work, so retry forever on a backoff
// rather than giving up: a router reboot must not take the device offline
// until someone power-cycles it. 1s doubling to 5 minutes.
#define WIFI_BACKOFF_MIN_MS         1000
#define WIFI_BACKOFF_MAX_MS         300000
// mDNS name. Registered once per boot, alongside a per-unit service instance
// so several units on one network stay individually discoverable.
#define MDNS_HOSTNAME               "cpapdash"
// Consecutive boots that fail to join the stored network before concluding the
// credentials are wrong and falling back to the setup portal. High on purpose:
// a router that is slow to come back after a power cut must not cost the user
// a re-provision, and each attempt is only a few seconds plus a reboot.
#define WIFI_FAIL_THRESHOLD         15
// Shorter budget when the failure looks like bad credentials. A typo should
// hand the setup page back in about a minute, not five. Not 1, because a
// marginal link produces the same reason codes with a correct password.
#define WIFI_AUTH_FAIL_THRESHOLD    4

// UART: TX=GPIO2, RX=GPIO3. Both boards use identical pins; the 3D-printed
// tape board does the TX->RX crossover via the 180-deg module layout.
// GPIO2 is a strapping pin but is always TX (idles high), so it stays boot-safe.
#define UART_PORT_NUM               UART_NUM_1
// There is no hardware flow control: the link is two wires. Reliability comes
// from acknowledging every chunk (see wait_chunk_ack on the miner), not from
// this number, and chunk CRCs catch whatever still slips through.
//
// This value is what one bench pair tolerated. It is a starting point, NOT a
// specification. How the two boards are joined is entirely up to whoever built
// the thing — jumper wires, copper tape, a fabricated board, ribbon of unknown
// length — and what one setup carries cleanly another will not. If /api/logs
// shows chunk CRC mismatches, lower it. Both boards must match, so changing it
// means reflashing the pair.
#define UART_BAUD_RATE              460800
#define UART_TX_PIN                 GPIO_NUM_2
#define UART_RX_PIN                 GPIO_NUM_3
// 32 KB, deliberately larger than the miner's. This is the direction the bulk
// data flows, and the httpd task must base64-decode, CRC and push each chunk
// over WiFi before it reads again — so any WiFi stall has to be absorbed here.
// With no hardware flow control on a two-wire link, ring depth IS the flow
// control. At 460800 this is ~700 ms of slack.
#define UART_RX_BUFFER_SIZE         32768
#define UART_TX_BUFFER_SIZE         8192
#define UART_QUEUE_SIZE             20
// Longest single newline-delimited frame. A proxy_chunk is PROXY_CHUNK_SIZE
// base64'd (4/3) plus the JSON envelope, so this must stay comfortably above
// PROXY_CHUNK_SIZE * 4 / 3.
#define UART_LINE_MAX               8192

// Mule task (boot-time config only)
#define MULE_TASK_STACK_SIZE        8192
#define MULE_TASK_PRIORITY          5
#define MULE_BOOT_DELAY_SEC         5

// Proxy configuration
#define PROXY_CHUNK_SIZE            4096
#define PROXY_UART_BUF_SIZE         8192
// How long the miner waits for the mule to acknowledge a chunk before giving
// up on the transfer. Generous: the mule may be blocked pushing the previous
// chunk to a slow HTTP client, which is exactly the condition this ack exists
// to absorb.
#define PROXY_CHUNK_ACK_TIMEOUT_MS  10000
#define PROXY_REQ_TIMEOUT_MS        30000   // per-frame UART recv timeout AND no-progress
                                            // stall window (held mutex). Generous so a
                                            // slow/flaky ezShare card does not 504 mid-
                                            // download; safe to be long now that the async
                                            // worker pool keeps the httpd task responsive.
#define PROXY_ABORT_DRAIN_MS        2000    // cap on draining the miner's leftover chunks
                                            // after a mid-stream client disconnect
#define PROXY_LOCK_ACQUIRE_MS       3000    // fast-fail budget for acquiring s_proxy_mutex. The
                                            // handlers run on the single httpd task, so a second
                                            // client must get an immediate 503 rather than pin
                                            // that task for the length of someone else's download.

// O2Ring UART timeouts (BLE operations are slower than ezShare HTTP)
#define O2RING_STATUS_TIMEOUT_MS    20000
#define O2RING_FILES_TIMEOUT_MS     25000
#define O2RING_LIVE_TIMEOUT_MS      25000
#define O2RING_DOWNLOAD_TIMEOUT_MS  120000

// Crash-loop self-heal. Six consecutive crash-boots is well past "unlucky" and
// into "this will not fix itself"; a device that has stayed up for a minute
// has cleared every boot-time fault worth counting.
#define CRASH_LOOP_THRESHOLD        6
#define CRASH_GUARD_HEALTHY_SEC     60

// Local HTTP server (control page + proxy routes share one httpd)
#define CONTROL_HTTP_PORT           80

// Captive portal
#define PORTAL_AP_CHANNEL           1
#define PORTAL_MAX_CONN             2

// Memory
#define JSON_BUFFER_SIZE            4096

// Log tags
#define LOG_TAG_MULE                "MULE"
#define LOG_TAG_WIFI                "WIFI"
#define LOG_TAG_UART                "UART"
