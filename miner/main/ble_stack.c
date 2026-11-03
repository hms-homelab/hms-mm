/**
 * @file ble_stack.c
 * @brief One NimBLE stack, shared by the ring and the machine (SDD-001).
 */
#include "ble_stack.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "ble_stack";

#define EVT_SYNC   (1 << 0)
#define SYNC_MS    5000

static SemaphoreHandle_t  s_mu;          /* acquire/release bookkeeping */
static EventGroupHandle_t s_evt;
static bool               s_up;
static uint8_t            s_users;       /* bit per ble_user_t */
static uint8_t            s_own_addr_type;

static portMUX_TYPE       s_master_mux = portMUX_INITIALIZER_UNLOCKED;
static ble_user_t         s_master_holder = BLE_USER_NONE;
static SemaphoreHandle_t  s_master_free;  /* given whenever the token is handed back */

static const char *user_name(ble_user_t who)
{
    switch (who) {
    case BLE_USER_RING: return "ring";
    case BLE_USER_AS11: return "as11";
    default:            return "none";
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed");
        return;
    }
    xEventGroupSetBits(s_evt, EVT_SYNC);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "NimBLE reset: %d", reason); }

static bool ensure_objects(void)
{
    if (!s_mu)          s_mu = xSemaphoreCreateMutex();
    if (!s_evt)         s_evt = xEventGroupCreate();
    if (!s_master_free) s_master_free = xSemaphoreCreateBinary();
    return s_mu && s_evt && s_master_free;
}

bool ble_stack_acquire(ble_user_t who)
{
    if (!ensure_objects()) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_up) {
        s_users |= (uint8_t)(1u << who);
        xSemaphoreGive(s_mu);
        return true;
    }

    xEventGroupClearBits(s_evt, EVT_SYNC);
    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "classic BT mem release: %s", esp_err_to_name(ret));
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_mu);
        return false;
    }
    /* Both modules set exactly these; set once here. */
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;
    nimble_port_freertos_init(host_task);
    EventBits_t b = xEventGroupWaitBits(s_evt, EVT_SYNC, pdTRUE, pdTRUE, pdMS_TO_TICKS(SYNC_MS));
    if (!(b & EVT_SYNC)) {
        ESP_LOGE(TAG, "NimBLE did not sync in %d ms", SYNC_MS);
        nimble_port_stop();
        nimble_port_deinit();
        xSemaphoreGive(s_mu);
        return false;
    }
    s_up = true;
    s_users = (uint8_t)(1u << who);
    s_master_holder = BLE_USER_NONE;
    ESP_LOGI(TAG, "up for %s (free=%lu largest=%u)", user_name(who),
             (unsigned long)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    xSemaphoreGive(s_mu);
    return true;
}

void ble_stack_release(ble_user_t who)
{
    if (!s_mu) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!s_up || !(s_users & (1u << who))) {
        xSemaphoreGive(s_mu);
        return;
    }
    s_users &= (uint8_t)~(1u << who);
    ble_stack_master_give(who);
    if (s_users) {
        ESP_LOGI(TAG, "%s let go; %s still holds the stack", user_name(who),
                 (s_users & (1u << BLE_USER_RING)) ? "ring" : "as11");
        xSemaphoreGive(s_mu);
        return;
    }
    nimble_port_stop();
    nimble_port_deinit();
    s_up = false;
    ESP_LOGI(TAG, "down after %s (free=%lu largest=%u)", user_name(who),
             (unsigned long)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    xSemaphoreGive(s_mu);
}

bool    ble_stack_is_up(void)        { return s_up; }
uint8_t ble_stack_own_addr_type(void) { return s_own_addr_type; }

int ble_stack_users(void)
{
    int n = 0;
    for (uint8_t b = s_users; b; b >>= 1) n += (b & 1);
    return n;
}

bool ble_stack_master_take(ble_user_t who, uint32_t wait_ms)
{
    if (!s_master_free) return false;
    const int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    for (;;) {
        portENTER_CRITICAL(&s_master_mux);
        const bool ok = (s_master_holder == BLE_USER_NONE || s_master_holder == who);
        if (ok) s_master_holder = who;
        portEXIT_CRITICAL(&s_master_mux);
        if (ok) return true;
        const int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            ESP_LOGW(TAG, "%s wanted the master token; %s holds it", user_name(who),
                     user_name(s_master_holder));
            return false;
        }
        /* Woken when the holder gives; the loop re-checks, so a third party
         * taking it first costs one more wait, not a wrong answer. */
        xSemaphoreTake(s_master_free, pdMS_TO_TICKS((uint32_t)(left_us / 1000) + 1));
    }
}

void ble_stack_master_give(ble_user_t who)
{
    if (!s_master_free) return;
    portENTER_CRITICAL(&s_master_mux);
    const bool was = (s_master_holder == who);
    if (was) s_master_holder = BLE_USER_NONE;
    portEXIT_CRITICAL(&s_master_mux);
    if (was) xSemaphoreGive(s_master_free);
}

ble_user_t ble_stack_master_holder(void) { return s_master_holder; }
