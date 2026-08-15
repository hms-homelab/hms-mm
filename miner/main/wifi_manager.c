/**
 * @file wifi_manager.c
 * @brief Scanner C3 WiFi Manager - ez Share WiFi Connection
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "config.h"

static const char *TAG = LOG_TAG_WIFI;

// Event group for WiFi events
static EventGroupHandle_t wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;

// Connection state
static wifi_status_t wifi_status = WIFI_STATUS_DISCONNECTED;
static int retry_count = 0;
static bool wifi_initialized = false;

/* Last raw WIFI_REASON_* from the driver. Reported to the mule alongside a
 * proxy error so an ezShare failure can be triaged without a serial cable:
 * 201 NO_AP_FOUND (card off, asleep or out of range) and 202 AUTH_FAIL (wrong
 * password) are entirely different problems that otherwise both surface as a
 * bare 502. */
static int last_disc_reason = 0;

/* True once this boot has associated at least once. Distinguishes "cannot find
 * the card" from "was talking to the card and lost it", which need opposite
 * retry behaviour. */
static bool ever_connected = false;
static bool wifi_started = false;

/* Current backoff delay; 0 means the ladder has not started. */
static uint32_t backoff_ms = 0;
static esp_timer_handle_t reconnect_timer = NULL;

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

static void log_disconnect(int reason)
{
    if (reason == suppressed_reason) {
        suppressed_count++;              /* same cause; summarise on change */
        return;
    }
    flush_disconnect_log();
    suppressed_reason = reason;
    ESP_LOGI(TAG, "WiFi disconnected (reason=%d)", reason);
}

static void reconnect_timer_cb(void *arg)
{
    /* Clear any stale association state before trying again. Without this the
     * supplicant can sit believing it is mid-association and the connect call
     * quietly does nothing. */
    esp_wifi_disconnect();
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    backoff_ms = (backoff_ms == 0) ? WIFI_BACKOFF_MIN_MS : backoff_ms * 2;
    if (backoff_ms > WIFI_BACKOFF_MAX_MS) backoff_ms = WIFI_BACKOFF_MAX_MS;

    if (!reconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback = reconnect_timer_cb, .name = "wifi_retry",
        };
        if (esp_timer_create(&args, &reconnect_timer) != ESP_OK) {
            esp_wifi_connect();          /* no timer: fall back to immediate */
            return;
        }
    }
    esp_timer_stop(reconnect_timer);
    esp_timer_start_once(reconnect_timer, (uint64_t)backoff_ms * 1000ULL);
}

/**
 * @brief WiFi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started, connecting...");
        esp_wifi_connect();
        wifi_status = WIFI_STATUS_CONNECTING;

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        if (d) last_disc_reason = d->reason;

        log_disconnect(last_disc_reason);

        if (!ever_connected && retry_count < WIFI_MAXIMUM_RETRY) {
            /* Initial connect: retry promptly. Nothing is established yet, so
             * there is no session on the card to protect. */
            esp_wifi_connect();
            retry_count++;
            wifi_status = WIFI_STATUS_CONNECTING;
            return;
        }

        if (!ever_connected) {
            ESP_LOGE(TAG, "WiFi connection failed after %d attempts (reason=%d)",
                     WIFI_MAXIMUM_RETRY, last_disc_reason);
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            wifi_status = WIFI_STATUS_ERROR;
            return;
        }

        /* Once we HAVE connected before, every later drop goes straight to
         * exponential backoff. The old code burst WIFI_MAXIMUM_RETRY immediate
         * reconnects on every single disconnect, forever, with no delay
         * between them. The ezShare card is a single-client soft AP: hammered
         * like that its session table wedges and it stops answering entirely,
         * which then looks like a dead card rather than a client that will not
         * stop knocking. */
        wifi_status = WIFI_STATUS_CONNECTING;
        schedule_reconnect();

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        backoff_ms  = 0;
        ever_connected = true;
        flush_disconnect_log();
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_status = WIFI_STATUS_CONNECTED;
    }
}

/**
 * @brief Initialize WiFi manager
 */
esp_err_t wifi_manager_init(void) {
    if (wifi_initialized) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return ESP_OK;
    }

    // Initialize NVS (required for WiFi)
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

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create WiFi station interface
    esp_netif_create_default_wifi_sta();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create event group
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    // Set WiFi mode to station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_initialized = true;
    wifi_status = WIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi manager initialized");

    return ESP_OK;
}

/**
 * @brief Connect to WiFi network
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (!wifi_initialized) {
        ESP_LOGE(TAG, "WiFi manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL) {
        ESP_LOGE(TAG, "NULL SSID");
        return ESP_ERR_INVALID_ARG;
    }

    // Configure WiFi connection
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    // Set WiFi configuration
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }

    // Clear event bits
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Start WiFi
    retry_count = 0;
    backoff_ms  = 0;
    ret = esp_wifi_start();
    if (ret == ESP_ERR_WIFI_NOT_STOPPED || wifi_started) {
        /* Already running, usually because the card dropped us rather than
         * because anyone called disconnect(). esp_wifi_start() is then a no-op,
         * STA_START never fires again, and nothing would reconnect: the caller
         * would just wait out its timeout. Force a fresh association. */
        ESP_LOGI(TAG, "WiFi already started, forcing reassociation");
        esp_wifi_disconnect();
        ret = esp_wifi_connect();
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    } else {
        wifi_started = true;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to (re)connect WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ~11 dBm. The C3 SuperMini's PCB antenna cannot take the default ~20 dBm:
     * driven that hard the output distorts, and the symptom is not "weak" but
     * "unintelligible" — the chip hears everything and nothing can decode what
     * it sends. In STA that looks like reaching auth and then AUTH_EXPIRE; in
     * AP mode it looks like beacons that no device on the bench can see. */
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed");
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
        return ESP_ERR_TIMEOUT;
    }
}

/**
 * @brief Disconnect from WiFi network
 */
esp_err_t wifi_manager_disconnect(void) {
    if (!wifi_initialized) {
        ESP_LOGW(TAG, "WiFi manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (wifi_status == WIFI_STATUS_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi already disconnected");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Disconnecting WiFi...");

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disconnect WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_status = WIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi disconnected");

    return ESP_OK;
}

/**
 * @brief Get current WiFi connection status
 */
wifi_status_t wifi_manager_get_status(void) {
    return wifi_status;
}

/**
 * @brief Check if WiFi is connected
 */
bool wifi_manager_is_connected(void) {
    return (wifi_status == WIFI_STATUS_CONNECTED);
}

/**
 * @brief Wait for WiFi connection (blocking)
 */
esp_err_t wifi_manager_wait_connection(uint32_t timeout_ms) {
    if (!wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    } else {
        return ESP_ERR_TIMEOUT;
    }
}

int wifi_manager_last_disc_reason(void) {
    return last_disc_reason;
}

int wifi_manager_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;   /* 0 = not associated */
    return ap.rssi;
}

/**
 * @brief Deinitialize WiFi manager
 */
void wifi_manager_deinit(void) {
    if (wifi_initialized) {
        wifi_manager_disconnect();
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
        esp_wifi_deinit();
        if (wifi_event_group != NULL) {
            vEventGroupDelete(wifi_event_group);
            wifi_event_group = NULL;
        }
        wifi_initialized = false;
        ESP_LOGI(TAG, "WiFi manager deinitialized");
    }
}
