/**
 * @file serial_config.c
 * @brief Provisioning over the mule's USB port, for the browser flasher.
 *
 * Reads newline-terminated JSON commands from the USB Serial/JTAG peripheral
 * and answers on the same link. Two commands:
 *
 *   {"cmd":"ping"}
 *     -> HMSMM {"ok":true,"role":"mule","fw":"1.0.1","serial":"MM-1A2B",
 *               "wifi":false,"ezshare":false}
 *
 *   {"cmd":"provision","ssid":"..","pass":"..","ez_ssid":"..","ez_pass":".."}
 *     -> HMSMM {"ok":true,"restarting":true}     (then reboots)
 *
 * Every reply is one line starting with "HMSMM ", because ESP_LOG output shares
 * this port: the browser scans for that prefix instead of assuming the next
 * line it reads is the answer. ping exists so the page can tell a mule from a
 * miner before it writes credentials to the wrong board.
 *
 * This talks to the USB Serial/JTAG driver directly rather than through stdin,
 * so it is independent of whatever the console is configured to use. The native
 * USB port is also the port the browser flashes through, which is what makes
 * "flash it, then hand it your WiFi" one cable and one page.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "config.h"
#include "nvs_config.h"
#include "serial_config.h"

static const char *TAG = "SERCFG";

// A provision command with the longest fields the rest of the firmware accepts
// (two 32-char SSIDs, two 64-char passwords) is well under 300 bytes. The cap
// exists so a browser that never sends a newline cannot grow this buffer.
#define LINE_MAX            512
#define REPLY_MAX           320
#define RX_CHUNK            64
#define TASK_STACK_SIZE     4096
#define TASK_PRIORITY       4
// Long enough for the reply to leave the USB FIFO before the reset kills it.
#define RESTART_DELAY_MS    600

static void reply(const char *json)
{
    char line[REPLY_MAX + 8];
    int n = snprintf(line, sizeof(line), "HMSMM %s\n", json);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    usb_serial_jtag_write_bytes((const uint8_t *)line, n, pdMS_TO_TICKS(1000));
}

static void reply_error(const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        reply(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

static void handle_ping(void)
{
    char serial[16] = {0};
    nvs_config_get_serial(serial, sizeof(serial));

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "role", "mule");
    cJSON_AddStringToObject(root, "fw", FW_VERSION);
    cJSON_AddStringToObject(root, "serial", serial);
    cJSON_AddBoolToObject(root, "wifi", nvs_config_has_wifi());
    cJSON_AddBoolToObject(root, "ezshare", nvs_config_has_ezshare());

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        reply(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

// Returns the string at `key`, or "" when it is absent or not a string, so a
// caller can always strlen the result.
static const char *str_field(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

// Returns true when the mule should restart to pick the new credentials up.
static bool handle_provision(const cJSON *root)
{
    const char *ssid = str_field(root, "ssid");
    const char *pass = str_field(root, "pass");
    const char *ez_ssid = str_field(root, "ez_ssid");
    const char *ez_pass = str_field(root, "ez_pass");

    // Length limits match the buffers everything downstream reads these into.
    if (strlen(ssid) == 0) {
        reply_error("ssid is required");
        return false;
    }
    if (strlen(ssid) > 32 || strlen(ez_ssid) > 32) {
        reply_error("ssid too long (max 32)");
        return false;
    }
    if (strlen(pass) > 64 || strlen(ez_pass) > 64) {
        reply_error("password too long (max 64)");
        return false;
    }

    nvs_config_set_wifi(ssid, pass);
    if (strlen(ez_ssid) > 0) {
        nvs_config_set_ezshare(ez_ssid, ez_pass);
    }

    // The miner gets its copy of the ezShare credentials from mule_task at the
    // next boot, so a restart is what actually finishes provisioning both
    // boards. Reporting it here keeps the browser from asking again.
    cJSON *out = cJSON_CreateObject();
    if (out) {
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddBoolToObject(out, "restarting", true);
        char *json = cJSON_PrintUnformatted(out);
        if (json) {
            reply(json);
            cJSON_free(json);
        }
        cJSON_Delete(out);
    }
    ESP_LOGI(TAG, "Provisioned over USB (SSID: %s) — restarting", ssid);
    return true;
}

// Returns true when the caller should restart the mule.
static bool handle_line(char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        reply_error("invalid JSON");
        return false;
    }

    bool restart = false;
    const char *cmd = str_field(root, "cmd");

    if (strcmp(cmd, "ping") == 0) {
        handle_ping();
    } else if (strcmp(cmd, "provision") == 0) {
        restart = handle_provision(root);
    } else {
        reply_error("unknown cmd");
    }

    cJSON_Delete(root);
    return restart;
}

static void serial_config_task(void *arg)
{
    static char line[LINE_MAX];
    size_t len = 0;
    bool overflowed = false;
    uint8_t chunk[RX_CHUNK];

    while (1) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];

            if (c != '\n' && c != '\r') {
                if (len < sizeof(line) - 1) {
                    line[len++] = c;
                } else {
                    // Keep draining to the newline; answer once, at the end.
                    overflowed = true;
                }
                continue;
            }

            if (overflowed) {
                reply_error("command too long");
            } else if (len > 0) {
                line[len] = '\0';
                if (handle_line(line)) {
                    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
                    esp_restart();
                }
            }
            len = 0;
            overflowed = false;
        }
    }
}

void serial_config_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "USB Serial/JTAG driver install failed: %s", esp_err_to_name(err));
        return;
    }

    if (xTaskCreate(serial_config_task, "sercfg", TASK_STACK_SIZE, NULL,
                    TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start serial config task");
        return;
    }
    ESP_LOGI(TAG, "USB provisioning ready");
}
