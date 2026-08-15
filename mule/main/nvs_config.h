#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NVS storage for mule: home WiFi + ezShare creds (to forward to miner).

void nvs_config_init(void);

// Home WiFi
bool nvs_config_has_wifi(void);
bool nvs_config_get_wifi_ssid(char *buf, size_t buf_size);
bool nvs_config_get_wifi_pass(char *buf, size_t buf_size);
void nvs_config_set_wifi(const char *ssid, const char *pass);

// ezShare creds (stored here + sent to miner via UART)
bool nvs_config_has_ezshare(void);
bool nvs_config_get_ezshare_ssid(char *buf, size_t buf_size);
bool nvs_config_get_ezshare_pass(char *buf, size_t buf_size);
void nvs_config_set_ezshare(const char *ssid, const char *pass);

// Device serial (auto-generated on first boot)
bool nvs_config_get_serial(char *buf, size_t buf_size);
void nvs_config_set_serial(const char *serial);

// Consecutive boots where the stored WiFi could not be joined. Cleared on any
// successful join. Used to decide when credentials are the problem.
uint32_t nvs_config_wifi_fail_bump(void);
uint32_t nvs_config_wifi_fail_count(void);
void nvs_config_wifi_fail_clear(void);

// Forget the home WiFi so the next boot lands on the captive portal. ezShare
// credentials and the serial are kept.
void nvs_config_clear_wifi(void);

// Forget everything, including the serial (which is regenerated on next boot).
void nvs_config_erase_all(void);

uint32_t nvs_config_increment_boot_count(void);
