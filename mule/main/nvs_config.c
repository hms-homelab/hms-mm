#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "nvs_cfg";
static nvs_handle_t s_nvs = 0;

static bool read_str(const char *key, char *buf, size_t buf_size)
{
    if (!s_nvs || !buf || buf_size == 0) return false;
    size_t len = buf_size;
    if (nvs_get_str(s_nvs, key, buf, &len) != ESP_OK) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

static void write_str(const char *key, const char *val)
{
    if (!s_nvs) return;
    nvs_set_str(s_nvs, key, val);
    nvs_commit(s_nvs);
}

void nvs_config_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_open("mule", NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    // Auto-generate serial on first boot
    char serial[16];
    if (!nvs_config_get_serial(serial, sizeof(serial))) {
        uint32_t r = esp_random();
        snprintf(serial, sizeof(serial), "MM-%04X", (unsigned)(r & 0xFFFF));
        nvs_config_set_serial(serial);
    }

    uint32_t boots = nvs_config_increment_boot_count();
    ESP_LOGI(TAG, "NVS init — serial %s, boot #%lu, wifi: %s, ezshare: %s",
             serial, (unsigned long)boots,
             nvs_config_has_wifi() ? "yes" : "no",
             nvs_config_has_ezshare() ? "yes" : "no");
}

bool nvs_config_has_wifi(void)     { char t[4]; return read_str("wifi_ssid", t, sizeof(t)); }
bool nvs_config_get_wifi_ssid(char *b, size_t s) { return read_str("wifi_ssid", b, s); }
bool nvs_config_get_wifi_pass(char *b, size_t s) { return read_str("wifi_pass", b, s); }
void nvs_config_set_wifi(const char *ssid, const char *pass)
{
    write_str("wifi_ssid", ssid);
    write_str("wifi_pass", pass);
    ESP_LOGI(TAG, "WiFi stored (SSID: %s)", ssid);
}

bool nvs_config_has_ezshare(void)  { char t[4]; return read_str("ez_ssid", t, sizeof(t)); }
bool nvs_config_get_ezshare_ssid(char *b, size_t s) { return read_str("ez_ssid", b, s); }
bool nvs_config_get_ezshare_pass(char *b, size_t s) { return read_str("ez_pass", b, s); }
void nvs_config_set_ezshare(const char *ssid, const char *pass)
{
    write_str("ez_ssid", ssid);
    write_str("ez_pass", pass);
    ESP_LOGI(TAG, "ezShare stored (SSID: %s)", ssid);
}

bool nvs_config_get_serial(char *b, size_t s) { return read_str("serial", b, s); }
void nvs_config_set_serial(const char *serial) { write_str("serial", serial); }

uint32_t nvs_config_increment_boot_count(void)
{
    if (!s_nvs) return 0;
    uint32_t count = 0;
    nvs_get_u32(s_nvs, "boot_count", &count);
    count++;
    nvs_set_u32(s_nvs, "boot_count", count);
    nvs_commit(s_nvs);
    return count;
}
