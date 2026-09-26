/**
 * @file wifi_manager.c
 * @brief Miner WiFi manager: the ezShare link, held.
 *
 * The link is started once at boot and held. It is never dropped for idleness
 * and never traded for BLE. Every failed join and every drop is retried in the
 * background on ez_retry_delay_ms() (5 s x 10, then exponential to 5 min, never
 * giving up), and no request ever waits for a join: a request that finds the
 * link down is answered at once, and the schedule keeps trying.
 *
 * Why held: the card is a single-client soft AP. Joining per request made
 * every request pay an association, and a card on an underpowered SD slot can
 * fail a re-association outright, so each join was a fresh chance to lose it.
 * Hammering it with immediate reconnects also wedges its session table, and it
 * then looks like a dead card rather than a client that will not stop knocking.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "ez_retry.h"
#include "config.h"

static const char *TAG = LOG_TAG_WIFI;

static EventGroupHandle_t wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT = BIT0;

static volatile wifi_status_t wifi_status = WIFI_STATUS_DISCONNECTED;
static volatile uint32_t s_retry_n = 0;      /* retries since the last GOT_IP */
static bool wifi_initialized = false;
static bool s_wifi_started = false;          /* esp_wifi_start() called, not since stopped */
static bool s_power_save_off = false;        /* WIFI_PS_NONE, see wifi_manager_set_power_save */

/* Last raw WIFI_REASON_* from the driver. Reported to the mule alongside a
 * proxy error so an ezShare failure can be triaged without a serial cable:
 * 201 NO_AP_FOUND (card off, asleep or out of range) and 202 AUTH_FAIL (wrong
 * password) are entirely different problems that otherwise both surface as a
 * bare 502. */
static volatile int last_disc_reason = 0;

/* Told when the link drops and when it is back (the scanner frees the ring's
 * stack on a drop, so the reassociation has the whole radio and heap). */
static wifi_link_cb_t s_link_cb = NULL;

static esp_timer_handle_t retry_timer = NULL;

/* Repeat-suppression for the disconnect log. A flapping link produces one line
 * per attempt, and at these intervals that is enough to push everything else
 * out of an 8 KB log ring long before anyone reads it. */
static int  suppressed_reason = 0;
static int  suppressed_count  = 0;

static void flush_disconnect_log(void)
{
    if (suppressed_count > 0)
        ESP_LOGI(TAG, "  [+%d more with reason=%d]", suppressed_count, suppressed_reason);
    suppressed_count = 0;
    suppressed_reason = 0;
}

static void log_disconnect(int reason, uint32_t retry_n, uint32_t delay_ms)
{
    if (reason == suppressed_reason) {
        suppressed_count++;              /* same cause; summarise on change */
        return;
    }
    flush_disconnect_log();
    suppressed_reason = reason;
    ESP_LOGW(TAG, "ezShare join failed/lost (reason=%d) -- retry %" PRIu32 " in %" PRIu32 " ms",
             reason, retry_n, delay_ms);
}

static void apply_radio_settings(void)
{
    /* Board-specific, see WIFI_TX_POWER_QDBM in config.h. Set on STA_START as
     * well, so the very first attempt already goes out at this level. */
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
    if (s_power_save_off) esp_wifi_set_ps(WIFI_PS_NONE);
}

static void retry_timer_cb(void *arg)
{
    /* Only armed from STA_DISCONNECTED, so the station is already down: there
     * is nothing to tear down first. */
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* Refused outright (a scan in progress, say): no STA_DISCONNECTED will
         * follow to arm the next try, so arm it here or the schedule stops. */
        ESP_LOGW(TAG, "esp_wifi_connect: %s -- retrying in %" PRIu32 " ms",
                 esp_err_to_name(err), ez_retry_delay_ms(s_retry_n));
        esp_timer_start_once(retry_timer, (uint64_t)ez_retry_delay_ms(s_retry_n) * 1000);
        return;
    }
    wifi_status = WIFI_STATUS_CONNECTING;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        apply_radio_settings();
        ESP_LOGI(TAG, "WiFi started (tx power %d/4 dBm), connecting...", WIFI_TX_POWER_QDBM);
        esp_wifi_connect();
        wifi_status = WIFI_STATUS_CONNECTING;

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        if (d) last_disc_reason = d->reason;
        bool was_up = (wifi_status == WIFI_STATUS_CONNECTED);
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_status = WIFI_STATUS_CONNECTING;
        if (was_up && s_link_cb) s_link_cb(false);

        /* A failed join and a drop take the same path. Never an immediate
         * retry: 5 s apart for 10 tries, then exponential to 5 min, and it
         * never gives up. */
        s_retry_n++;
        uint32_t delay_ms = ez_retry_delay_ms(s_retry_n);
        log_disconnect(last_disc_reason, s_retry_n, delay_ms);
        esp_timer_stop(retry_timer);   /* start_once refuses a timer already armed */
        esp_timer_start_once(retry_timer, (uint64_t)delay_ms * 1000);

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        flush_disconnect_log();
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        esp_timer_stop(retry_timer);
        s_retry_n = 0;               /* the next failure starts at try 1 */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_status = WIFI_STATUS_CONNECTED;
        if (s_link_cb) s_link_cb(true);
    }
}

esp_err_t wifi_manager_init(void) {
    if (wifi_initialized) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "ez_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &retry_timer));

    wifi_initialized = true;
    wifi_status = WIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

void wifi_manager_set_link_cb(wifi_link_cb_t cb)
{
    s_link_cb = cb;
}

void wifi_manager_set_power_save(bool off)
{
    s_power_save_off = off;
}

esp_err_t wifi_manager_start(const char *ssid, const char *password) {
    if (!wifi_initialized) {
        ESP_LOGE(TAG, "WiFi manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL) {
        ESP_LOGE(TAG, "NULL SSID");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wifi_started) return ESP_OK;   /* held: the schedule owns reconnection */

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    /* The ezShare runs WPA2 TKIP, which cannot do PMF. */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "Starting ezShare link, SSID: %s", ssid);
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }

    s_retry_n = 0;
    ret = esp_wifi_start();   /* fires STA_START, which makes the first attempt */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    }
    s_wifi_started = true;
    apply_radio_settings();
    return ESP_OK;
}

wifi_status_t wifi_manager_get_status(void) {
    return wifi_status;
}

bool wifi_manager_is_connected(void) {
    return (wifi_status == WIFI_STATUS_CONNECTED);
}

esp_err_t wifi_manager_wait_connection(uint32_t timeout_ms) {
    if (!wifi_initialized) return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

int wifi_manager_last_disc_reason(void) {
    return last_disc_reason;
}

int wifi_manager_rssi(void) {
    wifi_ap_record_t ap;
    if (wifi_status != WIFI_STATUS_CONNECTED) return 0;          /* 0 = not associated */
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

void wifi_manager_deinit(void) {
    if (!wifi_initialized) return;
    esp_timer_stop(retry_timer);
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_wifi_started = false;
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
    esp_wifi_deinit();
    esp_timer_delete(retry_timer);
    retry_timer = NULL;
    if (wifi_event_group != NULL) {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }
    wifi_initialized = false;
    wifi_status = WIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi manager deinitialized");
}
