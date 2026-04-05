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
    char tmp[4];
    return read_str("ez_ssid", tmp, sizeof(tmp));
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
