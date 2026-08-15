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

/**
 * Does this key hold a non-empty string?
 *
 * Asks NVS for the required length (out_value = NULL) rather than attempting a
 * read into a probe buffer. The previous form passed a 4-byte stack buffer,
 * and nvs_get_str returns ESP_ERR_NVS_INVALID_LENGTH — not ESP_OK — when the
 * buffer is too small, so ANY stored value of four characters or more was
 * reported as absent. For wifi_ssid that meant a provisioned mule went back to
 * the captive portal on every boot; for ez_ssid it meant the miner was never
 * sent its credentials.
 *
 * The returned length includes the terminator, so len <= 1 is an empty string.
 * Treating that as "not set" is what lets a reset clear a key by writing "".
 */
static bool has_str(const char *key)
{
    if (!s_nvs) return false;
    size_t len = 0;
    return nvs_get_str(s_nvs, key, NULL, &len) == ESP_OK && len > 1;
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

bool nvs_config_has_wifi(void)     { return has_str("wifi_ssid"); }
bool nvs_config_get_wifi_ssid(char *b, size_t s) { return read_str("wifi_ssid", b, s); }
bool nvs_config_get_wifi_pass(char *b, size_t s) { return read_str("wifi_pass", b, s); }
void nvs_config_set_wifi(const char *ssid, const char *pass)
{
    write_str("wifi_ssid", ssid);
    write_str("wifi_pass", pass);
    ESP_LOGI(TAG, "WiFi stored (SSID: %s)", ssid);
}

bool nvs_config_has_ezshare(void)  { return has_str("ez_ssid"); }
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

void nvs_config_clear_wifi(void)
{
    /* Clear the home WiFi only. ezShare credentials and the serial survive, so
     * a "reset WiFi" from the web UI puts the device back on the portal
     * without forgetting the card it was paired with. */
    write_str("wifi_ssid", "");
    write_str("wifi_pass", "");
    ESP_LOGW(TAG, "home WiFi credentials cleared");
}

void nvs_config_erase_all(void)
{
    if (!s_nvs) return;
    nvs_erase_all(s_nvs);
    nvs_commit(s_nvs);
    ESP_LOGW(TAG, "mule NVS erased");
}

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
