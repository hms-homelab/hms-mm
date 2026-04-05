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
