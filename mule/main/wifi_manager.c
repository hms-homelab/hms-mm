#include "wifi_manager.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_config.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = LOG_TAG_WIFI;

static EventGroupHandle_t s_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static int s_retry = 0;
static bool s_init = false;
static bool s_started = false;

/* Set once this boot has had an IP. Distinguishes "cannot join the network"
 * from "was on the network and lost it". */
static bool s_ever_connected = false;
static int  s_last_disc_reason = 0;

/* Sticky across a whole join attempt. The driver cycles through several reason
 * codes while failing (202, then 205, then 2 ...), so the LAST one is close to
 * a coin toss: classifying on it alone made the retry budget flip between the
 * patient and the impatient limit from boot to boot. If the AP rejected our
 * credentials even once, that is the fact worth keeping. */
static bool s_saw_auth_failure = false;

static uint32_t s_backoff_ms = 0;
static esp_timer_handle_t s_retry_timer = NULL;

/* mDNS is registered ONCE per boot. Re-registering on every reconnect logs
 * "Service already exists" and leaves the .local name unresolvable, so a unit
 * that has reconnected even once becomes unreachable by name. The component
 * re-announces on an address change by itself. */
static bool s_mdns_up = false;

static int s_suppressed_reason = 0;
static int s_suppressed_count  = 0;

static void flush_disconnect_log(void)
{
    if (s_suppressed_count > 0)
        ESP_LOGI(TAG, "  [+%d more with reason=%d]", s_suppressed_count, s_suppressed_reason);
    s_suppressed_count = 0;
    s_suppressed_reason = 0;
}

/* One line per distinct cause, then a count. A flapping link otherwise fills
 * the whole 8 KB log ring with identical lines. */
static void log_disconnect(int reason)
{
    if (reason == s_suppressed_reason) { s_suppressed_count++; return; }
    flush_disconnect_log();
    s_suppressed_reason = reason;
    ESP_LOGI(TAG, "WiFi disconnected (reason=%d)", reason);
}

static void retry_timer_cb(void *arg)
{
    esp_wifi_disconnect();      /* clear stale association state first */
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    s_backoff_ms = (s_backoff_ms == 0) ? WIFI_BACKOFF_MIN_MS : s_backoff_ms * 2;
    if (s_backoff_ms > WIFI_BACKOFF_MAX_MS) s_backoff_ms = WIFI_BACKOFF_MAX_MS;

    if (!s_retry_timer) {
        const esp_timer_create_args_t args = { .callback = retry_timer_cb, .name = "wifi_retry" };
        if (esp_timer_create(&args, &s_retry_timer) != ESP_OK) { esp_wifi_connect(); return; }
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000ULL);
}

static void mdns_start_once(void)
{
    if (s_mdns_up) return;
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(MDNS_HOSTNAME);

    /* Also advertise a per-unit service instance. Two of these on one network
     * both want the same hostname, and only one can have it; the instance name
     * is keyed by serial so each stays individually discoverable regardless. */
    char serial[24] = {0};
    nvs_config_get_serial(serial, sizeof(serial));
    char instance[48];
    snprintf(instance, sizeof(instance), "%s %s", FW_PROJECT, serial[0] ? serial : "unknown");

    mdns_txt_item_t txt[] = {
        { "serial", serial[0] ? serial : "unknown" },
        { "fw",     FW_VERSION },
    };
    if (mdns_service_add(instance, "_" FW_PROJECT, "_tcp", CONTROL_HTTP_PORT,
                         txt, sizeof(txt) / sizeof(txt[0])) == ESP_OK)
        ESP_LOGI(TAG, "mDNS: %s.local and %s over _%s._tcp",
                 MDNS_HOSTNAME, instance, FW_PROJECT);
    else
        ESP_LOGI(TAG, "mDNS hostname: %s.local", MDNS_HOSTNAME);

    s_mdns_up = true;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        s_status = WIFI_STATUS_CONNECTING;

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (d) s_last_disc_reason = d->reason;
        switch (s_last_disc_reason) {
            case 2: case 15: case 202: case 203: case 204:
                s_saw_auth_failure = true;
                break;
            default: break;
        }
        log_disconnect(s_last_disc_reason);

        if (!s_ever_connected && s_retry < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();          /* first join: retry promptly */
            s_retry++;
            s_status = WIFI_STATUS_CONNECTING;
            return;
        }
        if (!s_ever_connected) {
            /* Give up on THIS attempt so the caller can fall back to the setup
             * portal rather than blocking forever on a network that is not
             * there or whose password changed. */
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            s_status = WIFI_STATUS_ERROR;
            return;
        }

        /* Already had an IP, so the network exists and the credentials work;
         * something transient took it away (a router reboot, a roam, a power
         * cut). Keep retrying on a backoff ladder forever. Previously this
         * path stopped after WIFI_MAXIMUM_RETRY and left the device parked in
         * ERROR with nothing to bring it back, so a router reboot took the
         * mule offline until someone power-cycled it. */
        s_status = WIFI_STATUS_CONNECTING;
        schedule_reconnect();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_backoff_ms = 0;
        s_ever_connected = true;
        flush_disconnect_log();
        s_status = WIFI_STATUS_CONNECTED;
        mdns_start_once();
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
    cfg.sta.ssid[sizeof(cfg.sta.ssid) - 1] = '\0';
    if (password) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
        cfg.sta.password[sizeof(cfg.sta.password) - 1] = '\0';
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry = 0;
    s_backoff_ms = 0;
    s_saw_auth_failure = false;          /* per attempt, not per boot */
    esp_err_t rc = esp_wifi_start();
    if (rc == ESP_ERR_WIFI_NOT_STOPPED || s_started) {
        /* Already running. esp_wifi_start() is then a no-op and STA_START
         * never fires again, so nothing would reconnect and this call would
         * simply wait out its timeout. Force a fresh association. */
        ESP_LOGI(TAG, "WiFi already started, forcing reassociation");
        esp_wifi_disconnect();
        esp_wifi_connect();
    } else if (rc == ESP_OK) {
        s_started = true;
    }

    /* ~11 dBm. The C3 SuperMini's PCB antenna cannot take the default ~20 dBm:
     * driven that hard the output distorts, and the symptom is not "weak" but
     * "unintelligible" — the chip hears everything and nothing can decode what
     * it sends. In STA that looks like reaching auth and then AUTH_EXPIRE; in
     * AP mode it looks like beacons that no device on the bench can see. */
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    return ESP_FAIL;
}

int wifi_manager_last_disc_reason(void) { return s_last_disc_reason; }

bool wifi_manager_last_failure_was_auth(void)
{
    /* True if ANY attempt in this join was rejected on credentials. A run of
     * failures that never once produced an auth code (201 NO_AP_FOUND and
     * friends) means the network simply is not there, which a power cut
     * explains as well as a typo, so those stay patient. */
    return s_saw_auth_failure;
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
