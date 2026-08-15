#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvs_cfg";
static nvs_handle_t s_nvs = 0;

static bool read_str(const char *key, char *buf, size_t buf_size)
{
    if (!s_nvs || !buf || buf_size == 0) return false;
    size_t len = buf_size;
    esp_err_t ret = nvs_get_str(s_nvs, key, buf, &len);
    if (ret != ESP_OK) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

/* Does this key hold a non-empty string?
 *
 * Asks NVS for the required length rather than reading into a probe buffer.
 * The previous form passed a 4-byte stack buffer, and nvs_get_str returns
 * ESP_ERR_NVS_INVALID_LENGTH — not ESP_OK — when the buffer is too small, so
 * ANY stored SSID of four characters or more read as absent. The miner
 * therefore always fell back to the compiled-in defaults and never used the
 * credentials the mule had sent it. Invisible with a stock "ez Share" card,
 * and total failure with a renamed one. Same bug as the mule had. */
static bool has_str(const char *key)
{
    if (!s_nvs) return false;
    size_t len = 0;
    return nvs_get_str(s_nvs, key, NULL, &len) == ESP_OK && len > 1;
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

    ret = nvs_open("miner", NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "NVS config initialized (ezShare creds: %s)",
             nvs_config_has_ezshare() ? "stored" : "using defaults");
}

bool nvs_config_has_ezshare(void)
{
    return has_str("ez_ssid");
}

bool nvs_config_get_ezshare_ssid(char *buf, size_t buf_size)
{
    return read_str("ez_ssid", buf, buf_size);
}

bool nvs_config_get_ezshare_pass(char *buf, size_t buf_size)
{
    return read_str("ez_pass", buf, buf_size);
}

void nvs_config_set_ezshare(const char *ssid, const char *pass)
{
    write_str("ez_ssid", ssid);
    write_str("ez_pass", pass);
    ESP_LOGI(TAG, "ezShare credentials stored (SSID: %s)", ssid);
}

bool nvs_config_ble_active(void)
{
    /* Default OFF: bringing up BLE for the O2Ring disconnects the ezShare WiFi
     * link (shared radio on the C3), which interrupts CPAP data collection. Ship
     * disabled; set NVS miner/ble_active=1 (then reboot) to enable for dev. */
    if (!s_nvs) return false;
    uint8_t v = 0;
    return (nvs_get_u8(s_nvs, "ble_active", &v) == ESP_OK) && (v != 0);
}

void nvs_config_set_ble_active(bool enabled)
{
    if (!s_nvs) return;
    nvs_set_u8(s_nvs, "ble_active", enabled ? 1 : 0);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "ble_active set to %d (takes effect on reboot)", enabled ? 1 : 0);
}

void nvs_config_erase_all(void)
{
    /* Wipes this namespace only — ezShare creds and the BLE gate. The mule
     * re-pushes credentials with set_config on its next boot, so a reset unit
     * comes back configured rather than needing USB. */
    if (!s_nvs) return;
    nvs_erase_all(s_nvs);
    nvs_commit(s_nvs);
    ESP_LOGW(TAG, "miner NVS erased");
}
