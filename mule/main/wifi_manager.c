#include "wifi_manager.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = LOG_TAG_WIFI;

static EventGroupHandle_t s_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static int s_retry = 0;
static bool s_init = false;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        s_status = WIFI_STATUS_CONNECTING;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry++;
        } else {
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            s_status = WIFI_STATUS_ERROR;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_status = WIFI_STATUS_CONNECTED;
        mdns_init();
        mdns_hostname_set("cpapdash");
        ESP_LOGI(TAG, "mDNS hostname: cpapdash.local");
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_init) return ESP_OK;

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    s_event_group = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);

    s_init = true;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!s_init) return ESP_ERR_INVALID_STATE;

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry = 0;
    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    return ESP_FAIL;
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_init || s_status == WIFI_STATUS_DISCONNECTED) return ESP_OK;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_status = WIFI_STATUS_DISCONNECTED;
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_status == WIFI_STATUS_CONNECTED;
}

void wifi_manager_deinit(void)
{
    if (!s_init) return;
    wifi_manager_disconnect();
    esp_wifi_deinit();
    s_init = false;
}
