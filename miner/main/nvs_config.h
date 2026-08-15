#pragma once

#include <stdbool.h>
#include <stddef.h>

// NVS storage for ezShare credentials on the miner.
// Set by mule via UART set_config message, persists across reboots.

void nvs_config_init(void);

bool nvs_config_has_ezshare(void);
bool nvs_config_get_ezshare_ssid(char *buf, size_t buf_size);
bool nvs_config_get_ezshare_pass(char *buf, size_t buf_size);
void nvs_config_set_ezshare(const char *ssid, const char *pass);

// O2Ring BLE gate. Default false (off): BLE shares the C3 radio with ezShare WiFi,
// so bringing it up drops the CPAP data link. Set NVS miner/ble_active=1 to enable.
bool nvs_config_ble_active(void);

// Owner kill switch, driven from the mule over the link (o2_set_enabled) and
// ultimately from the web UI. Takes effect on reboot, because the BLE stack is
// only brought up (or not) at init.
void nvs_config_set_ble_active(bool enabled);

// Erase this namespace. Used by the link's `reset` message; the mule re-pushes
// ezShare credentials via set_config on its next boot.
void nvs_config_erase_all(void);
