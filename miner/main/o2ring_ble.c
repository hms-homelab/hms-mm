/**
 * @file o2ring_ble.c
 * @brief Wellue O2 Ring BLE GATT client
 *
 * Viatom protocol: 0xAA framing, CRC-8.
 * Commands: INFO (0x14), FILE_OPEN (0x03), FILE_READ (0x04), FILE_CLOSE (0x05).
 *
 * BLE service:  14839ac4-7d7e-415c-9a42-167340cf2339
 * Write char:   8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3
 * Notify char:  0734594a-a8e7-4b1a-a6b1-cd5243059a57
 */

#include "o2ring_ble.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "o2ring";

/* ── UUIDs (128-bit, little-endian byte order) ── */

static const uint8_t SVC_UUID[16] = {
    0x39, 0x23, 0xcf, 0x40, 0x73, 0x16, 0x42, 0x9a,
    0x5c, 0x41, 0x7e, 0x7d, 0xc4, 0x9a, 0x83, 0x14
};
static const uint8_t WRITE_UUID[16] = {
    0xa3, 0xe1, 0x26, 0x0a, 0xee, 0x9a, 0xe9, 0xbb,
    0xb0, 0x49, 0x0b, 0xeb, 0xe7, 0xac, 0x00, 0x8b
};
static const uint8_t NOTIFY_UUID[16] = {
    0x57, 0x9a, 0x05, 0x43, 0x52, 0xcd, 0xb1, 0xa6,
    0x1a, 0x4b, 0xe7, 0xa8, 0x4a, 0x59, 0x34, 0x07
};

/* ── Viatom protocol constants ── */

#define VIATOM_REQ_HDR   0xAA  /* host → device */
#define VIATOM_RESP_HDR  0x55  /* device → host */
#define CMD_INFO          0x14
#define CMD_READ_SENSORS  0x17
#define CMD_FILE_OPEN     0x03
#define CMD_FILE_READ     0x04
#define CMD_FILE_CLOSE    0x05

/* ── BLE state ── */

static esp_gatt_if_t g_if = ESP_GATT_IF_NONE;
static uint16_t g_conn_id;
static esp_bd_addr_t g_mac;
static bool g_connected = false;
static bool g_scanning = false;
static bool g_ready = false;  /* notifications enabled, can send commands */
static bool g_auto_reconnect = true;

static uint16_t g_svc_start, g_svc_end;
static uint16_t g_write_handle, g_notify_handle;

/* ── Synchronization for blocking commands ── */

static SemaphoreHandle_t g_cmd_mutex;     /* only one command at a time */
static EventGroupHandle_t g_cmd_events;
#define EVT_RESPONSE_READY  (1 << 0)
#define EVT_CONNECTED       (1 << 1)
#define EVT_CLOSE_ACK       (1 << 2)

/* Cached live reading */
static o2ring_live_t g_live = {0};

/* Response buffer — filled by notification handler, read by command caller */
#define RESP_BUF_SIZE  4096
static uint8_t g_resp_buf[RESP_BUF_SIZE];
static size_t  g_resp_len;

/* Frame reassembly buffer */
#define REASM_BUF_SIZE 4096
static uint8_t g_reasm[REASM_BUF_SIZE];
static size_t  g_reasm_len;

/* File download state — shared between command caller and notification handler */
static uint8_t *g_dl_buf;
static size_t   g_dl_buf_size;
static size_t   g_dl_offset;
static uint32_t g_dl_file_size;
static uint16_t g_dl_block;
static bool     g_dl_active;

/* Streaming callback — if set, dispatch_frame calls this instead of buffering */
static o2ring_chunk_cb_t g_dl_cb;
static void *g_dl_cb_ctx;
static bool g_dl_cb_error;

/* Cached device info */
static o2ring_device_info_t g_info;

/* ── CRC-8 (Viatom custom polynomial) ── */

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t chk = crc ^ data[i];
        crc = 0;
        if (chk & 0x01) crc  = 0x07;
        if (chk & 0x02) crc ^= 0x0e;
        if (chk & 0x04) crc ^= 0x1c;
        if (chk & 0x08) crc ^= 0x38;
        if (chk & 0x10) crc ^= 0x70;
        if (chk & 0x20) crc ^= 0xe0;
        if (chk & 0x40) crc ^= 0xc7;
        if (chk & 0x80) crc ^= 0x89;
    }
    return crc;
}

/* ── Build and send a Viatom command ── */

static uint8_t s_cmd_frame[512];

static esp_err_t send_cmd(uint8_t cmd, uint16_t block,
                           const uint8_t *payload, uint16_t payload_len)
{
    if (!g_ready || g_write_handle == 0) return ESP_ERR_INVALID_STATE;

    size_t frame_len = 7 + payload_len + 1;
    if (frame_len > sizeof(s_cmd_frame)) return ESP_ERR_INVALID_SIZE;
    uint8_t *frame = s_cmd_frame;

    frame[0] = VIATOM_REQ_HDR;
    frame[1] = cmd;
    frame[2] = cmd ^ 0xFF;
    frame[3] = block & 0xFF;
    frame[4] = (block >> 8) & 0xFF;
    frame[5] = payload_len & 0xFF;
    frame[6] = (payload_len >> 8) & 0xFF;
    if (payload && payload_len > 0)
        memcpy(frame + 7, payload, payload_len);
    frame[frame_len - 1] = crc8(frame, frame_len - 1);

    /* BLE write may need chunking at MTU boundary (20 bytes default) */
    size_t sent = 0;
    esp_err_t ret = ESP_OK;
    while (sent < frame_len) {
        size_t chunk = frame_len - sent;
        if (chunk > 20) chunk = 20;
        ret = esp_ble_gattc_write_char(g_if, g_conn_id, g_write_handle,
                                        chunk, frame + sent,
                                        ESP_GATT_WRITE_TYPE_RSP,
                                        ESP_GATT_AUTH_REQ_NONE);
        if (ret != ESP_OK) break;
        sent += chunk;
        if (sent < frame_len) vTaskDelay(pdMS_TO_TICKS(30));
    }

    return ret;
}

/* ── Frame reassembly + dispatch ── */

static void dispatch_frame(const uint8_t *frame, size_t len)
{
    if (len < 8) return;
    uint16_t payload_len = frame[5] | ((uint16_t)frame[6] << 8);
    if (7 + payload_len > len) {
        ESP_LOGW(TAG, "Truncated frame: payload_len=%u but frame=%zu", payload_len, len);
        return;
    }

    if (g_dl_active) {
        const uint8_t *data = frame + 7;

        if (g_dl_cb) {
            /* Streaming mode — fire callback per block */
            if (!g_dl_cb(data, payload_len, g_dl_offset, g_dl_cb_ctx)) {
                g_dl_cb_error = true;
                xEventGroupSetBits(g_cmd_events, EVT_RESPONSE_READY);
                return;
            }
        } else if (g_dl_buf) {
            /* Buffer mode — append to caller buffer */
            size_t to_copy = payload_len;
            if (g_dl_offset + to_copy > g_dl_buf_size)
                to_copy = g_dl_buf_size - g_dl_offset;
            if (to_copy > 0)
                memcpy(g_dl_buf + g_dl_offset, data, to_copy);
        }

        g_dl_offset += payload_len;

        if (g_dl_offset >= g_dl_file_size) {
            xEventGroupSetBits(g_cmd_events, EVT_RESPONSE_READY);
        } else {
            g_dl_block++;
            send_cmd(CMD_FILE_READ, g_dl_block, NULL, 0);
        }
        return;
    }

    /* For INFO and FILE_OPEN responses, copy to response buffer */
    if (payload_len <= RESP_BUF_SIZE && 7 + payload_len <= len) {
        memcpy(g_resp_buf, frame + 7, payload_len);
        g_resp_len = payload_len;
    } else {
        ESP_LOGW(TAG, "Frame payload exceeds buffer (pl=%u buf=%d frame=%zu)",
                 payload_len, RESP_BUF_SIZE, len);
        g_resp_len = 0;
    }
    uint8_t resp_cmd = frame[1];
    EventBits_t bits_to_set = EVT_RESPONSE_READY;
    if (resp_cmd == CMD_FILE_CLOSE) bits_to_set |= EVT_CLOSE_ACK;
    xEventGroupSetBits(g_cmd_events, bits_to_set);
}

static void process_notification(const uint8_t *data, size_t len)
{
    if (g_reasm_len + len > REASM_BUF_SIZE)
        g_reasm_len = 0;
    memcpy(g_reasm + g_reasm_len, data, len);
    g_reasm_len += len;

    while (g_reasm_len > 0) {
        /* Resync to 0xAA (request echo) or 0x55 (response) */
        while (g_reasm_len > 0 &&
               g_reasm[0] != VIATOM_REQ_HDR && g_reasm[0] != VIATOM_RESP_HDR) {
            memmove(g_reasm, g_reasm + 1, --g_reasm_len);
        }
        if (g_reasm_len < 7) return;

        if ((g_reasm[1] ^ 0xFF) != g_reasm[2]) {
            memmove(g_reasm, g_reasm + 1, --g_reasm_len);
            continue;
        }

        uint16_t plen = g_reasm[5] | ((uint16_t)g_reasm[6] << 8);
        size_t total = 7 + plen + 1;
        if (g_reasm_len < total) return;

        uint8_t want = g_reasm[total - 1];
        uint8_t got  = crc8(g_reasm, total - 1);
        if (want != got) {
            ESP_LOGW(TAG, "CRC fail: want=0x%02x got=0x%02x", want, got);
            memmove(g_reasm, g_reasm + 1, --g_reasm_len);
            continue;
        }

        dispatch_frame(g_reasm, total);

        if (g_reasm_len > total)
            memmove(g_reasm, g_reasm + total, g_reasm_len - total);
        g_reasm_len -= total;
    }
}

/* ── Send command and wait for response ── */

static esp_err_t send_and_wait(uint8_t cmd, uint16_t block,
                                const uint8_t *payload, uint16_t payload_len,
                                uint32_t timeout_ms)
{
    xEventGroupClearBits(g_cmd_events, EVT_RESPONSE_READY);
    g_resp_len = 0;

    esp_err_t ret = send_cmd(cmd, block, payload, payload_len);
    if (ret != ESP_OK) return ret;

    EventBits_t bits = xEventGroupWaitBits(g_cmd_events, EVT_RESPONSE_READY,
                                            pdTRUE, pdTRUE,
                                            pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EVT_RESPONSE_READY)) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

/* ── UUID match helper ── */

static bool uuid128_eq(const esp_bt_uuid_t *u, const uint8_t *target)
{
    return u->len == ESP_UUID_LEN_128 && memcmp(u->uuid.uuid128, target, 16) == 0;
}

/* ── GAP handler ── */

static void gap_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(0);
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (p->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Scanning for O2 Ring...");
            g_scanning = true;
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (p->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;

        uint8_t *name = NULL;
        uint8_t name_len = 0;
        name = esp_ble_resolve_adv_data(p->scan_rst.ble_adv,
                                         ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
        if (!name)
            name = esp_ble_resolve_adv_data(p->scan_rst.ble_adv,
                                             ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);

        bool match = false;
        if (name && name_len > 0) {
            char s[32] = {0};
            memcpy(s, name, name_len < 31 ? name_len : 31);
            if (strstr(s, "O2Ring")) {
                ESP_LOGI(TAG, "Found: %s RSSI=%d", s, p->scan_rst.rssi);
                match = true;
            }
        }

        if (match) {
            esp_ble_gap_stop_scanning();
            g_scanning = false;
            memcpy(g_mac, p->scan_rst.bda, 6);
            ESP_LOGI(TAG, "Connecting to %02x:%02x:%02x:%02x:%02x:%02x",
                     g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
            esp_ble_gattc_open(g_if, p->scan_rst.bda,
                               p->scan_rst.ble_addr_type, true);
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        g_scanning = false;
        break;

    default:
        break;
    }
}

/* ── GATTC handler ── */

static void gattc_handler(esp_gattc_cb_event_t event,
                           esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *p)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        g_if = gattc_if;
        esp_ble_gap_set_scan_params(&(esp_ble_scan_params_t){
            .scan_type          = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval      = 0x80,
            .scan_window        = 0x40,
        });
        break;

    case ESP_GATTC_OPEN_EVT:
        if (p->open.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Connected");
            g_conn_id = p->open.conn_id;
            g_connected = true;
            g_reasm_len = 0;
            esp_ble_gattc_search_service(gattc_if, g_conn_id, NULL);
        } else {
            ESP_LOGE(TAG, "Connect failed: %d", p->open.status);
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_ble_gap_start_scanning(0);
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (uuid128_eq(&p->search_res.srvc_id.uuid, SVC_UUID)) {
            g_svc_start = p->search_res.start_handle;
            g_svc_end = p->search_res.end_handle;
            ESP_LOGI(TAG, "Viatom service 0x%04x-0x%04x", g_svc_start, g_svc_end);
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (!g_svc_start) {
            ESP_LOGE(TAG, "Viatom service not found");
            esp_ble_gattc_close(gattc_if, g_conn_id);
            break;
        }

        uint16_t count = 0;
        esp_ble_gattc_get_attr_count(gattc_if, g_conn_id,
                                      ESP_GATT_DB_CHARACTERISTIC,
                                      g_svc_start, g_svc_end, 0, &count);
        if (!count) break;

        esp_gattc_char_elem_t *ch = malloc(sizeof(esp_gattc_char_elem_t) * count);
        if (!ch) break;
        esp_ble_gattc_get_all_char(gattc_if, g_conn_id,
                                    g_svc_start, g_svc_end, ch, &count, 0);

        for (int i = 0; i < count; i++) {
            if (uuid128_eq(&ch[i].uuid, WRITE_UUID)) {
                g_write_handle = ch[i].char_handle;
                ESP_LOGI(TAG, "Write handle: 0x%04x", g_write_handle);
            }
            if (uuid128_eq(&ch[i].uuid, NOTIFY_UUID)) {
                g_notify_handle = ch[i].char_handle;
                ESP_LOGI(TAG, "Notify handle: 0x%04x", g_notify_handle);
                esp_ble_gattc_register_for_notify(gattc_if, g_mac, g_notify_handle);
            }
        }
        free(ch);
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (p->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Register notify failed: %d", p->reg_for_notify.status);
            break;
        }

        /* Find CCCD descriptor and write 0x0001 to enable notifications */
        uint16_t dcount = 0;
        esp_ble_gattc_get_attr_count(gattc_if, g_conn_id,
                                      ESP_GATT_DB_DESCRIPTOR,
                                      g_svc_start, g_svc_end,
                                      g_notify_handle, &dcount);
        if (!dcount) break;

        esp_gattc_descr_elem_t *desc = malloc(sizeof(esp_gattc_descr_elem_t) * dcount);
        if (!desc) break;
        esp_ble_gattc_get_all_descr(gattc_if, g_conn_id,
                                     g_notify_handle, desc, &dcount, 0);
        for (int i = 0; i < dcount; i++) {
            if (desc[i].uuid.len == ESP_UUID_LEN_16 &&
                desc[i].uuid.uuid.uuid16 == 0x2902) {
                uint16_t en = 0x0001;
                esp_ble_gattc_write_char_descr(gattc_if, g_conn_id, desc[i].handle,
                                                sizeof(en), (uint8_t *)&en,
                                                ESP_GATT_WRITE_TYPE_RSP,
                                                ESP_GATT_AUTH_REQ_NONE);
                ESP_LOGI(TAG, "Enabling CCCD 0x%04x", desc[i].handle);
                break;
            }
        }
        free(desc);
        break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (p->write.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Notifications enabled — ready for commands");
            g_ready = true;
            xEventGroupSetBits(g_cmd_events, EVT_CONNECTED);
        }
        break;

    case ESP_GATTC_NOTIFY_EVT:
        process_notification(p->notify.value, p->notify.value_len);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "Disconnected (reason=%d)", p->disconnect.reason);
        g_connected = false;
        g_ready = false;
        g_write_handle = 0;
        g_notify_handle = 0;
        g_svc_start = g_svc_end = 0;
        g_reasm_len = 0;
        if (g_dl_active) {
            g_dl_cb_error = true;
            g_dl_active = false;
            xEventGroupSetBits(g_cmd_events, EVT_RESPONSE_READY);
        } else {
            g_dl_active = false;
        }
        g_dl_buf = NULL;
        g_dl_cb = NULL;
        g_dl_buf_size = 0;
        g_dl_offset = 0;
        g_info.valid = false;

        if (g_auto_reconnect && g_if != ESP_GATT_IF_NONE) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            ESP_LOGI(TAG, "Restarting scan...");
            esp_ble_gap_start_scanning(0);
        }
        break;

    default:
        break;
    }
}

/* ── Public API ── */

esp_err_t o2ring_ble_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE");

    g_cmd_mutex = xSemaphoreCreateMutex();
    g_cmd_events = xEventGroupCreate();
    if (!g_cmd_mutex || !g_cmd_events) return ESP_ERR_NO_MEM;

    memset(&g_info, 0, sizeof(g_info));

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&cfg);
    if (ret) { ESP_LOGE(TAG, "BT init: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) { ESP_LOGE(TAG, "BT enable: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_bluedroid_init();
    if (ret) { ESP_LOGE(TAG, "BD init: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_bluedroid_enable();
    if (ret) { ESP_LOGE(TAG, "BD enable: %s", esp_err_to_name(ret)); return ret; }

    esp_ble_gattc_register_callback(gattc_handler);
    esp_ble_gap_register_callback(gap_handler);
    esp_ble_gattc_app_register(0);
    esp_ble_gatt_set_local_mtu(247);

    ESP_LOGI(TAG, "BLE ready");
    return ESP_OK;
}

void o2ring_ble_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing BLE");
    if (g_connected) {
        esp_ble_gattc_close(g_if, g_conn_id);
        g_connected = false;
    }
    if (g_scanning) {
        esp_ble_gap_stop_scanning();
        g_scanning = false;
    }
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    g_if = ESP_GATT_IF_NONE;
    g_conn_id = 0;
    g_write_handle = 0;
    g_notify_handle = 0;
    ESP_LOGI(TAG, "BLE deinitialized — memory released");
}

esp_err_t o2ring_ble_start_scan(void)
{
    if (g_if == ESP_GATT_IF_NONE) return ESP_ERR_INVALID_STATE;
    return esp_ble_gap_start_scanning(0);
}

esp_err_t o2ring_ble_stop(void)
{
    if (g_scanning) esp_ble_gap_stop_scanning();
    if (g_connected) esp_ble_gattc_close(g_if, g_conn_id);
    return ESP_OK;
}

bool o2ring_ble_is_connected(void)
{
    return g_ready;
}

esp_err_t o2ring_ble_refresh_info(void)
{
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(g_cmd_mutex, pdMS_TO_TICKS(15000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    g_info.valid = false;
    esp_err_t ret = send_and_wait(CMD_INFO, 0, NULL, 0, 10000);
    if (ret != ESP_OK) {
        xSemaphoreGive(g_cmd_mutex);
        return ret;
    }

    /* INFO response is a JSON string in g_resp_buf, possibly null-padded.
     * Find actual JSON length (ends at last '}' before null padding). */
    size_t json_len = g_resp_len;
    while (json_len > 0 && g_resp_buf[json_len - 1] == 0x00)
        json_len--;
    if (json_len == 0) {
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char *json_str = malloc(json_len + 1);
    if (!json_str) { xSemaphoreGive(g_cmd_mutex); return ESP_ERR_NO_MEM; }
    memcpy(json_str, g_resp_buf, json_len);
    json_str[json_len] = '\0';

    ESP_LOGI(TAG, "INFO response: %s", json_str);

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse INFO JSON");
        free(json_str);
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *bat = cJSON_GetObjectItem(root, "CurBAT");
    if (bat) {
        if (cJSON_IsNumber(bat)) {
            g_info.battery = (uint8_t)bat->valueint;
        } else if (cJSON_IsString(bat)) {
            g_info.battery = (uint8_t)atoi(bat->valuestring);
        }
    }

    cJSON *model = cJSON_GetObjectItem(root, "Model");
    if (model && cJSON_IsString(model))
        strncpy(g_info.model, model->valuestring, sizeof(g_info.model) - 1);

    cJSON *sn = cJSON_GetObjectItem(root, "SN");
    if (sn && cJSON_IsString(sn))
        strncpy(g_info.serial, sn->valuestring, sizeof(g_info.serial) - 1);

    cJSON *fl = cJSON_GetObjectItem(root, "FileList");
    g_info.file_count = 0;
    if (fl && cJSON_IsString(fl) && strlen(fl->valuestring) > 0) {
        char *list = strdup(fl->valuestring);
        if (list) {
            char *tok = strtok(list, ",");
            while (tok && g_info.file_count < O2RING_MAX_FILES) {
                while (*tok == ' ') tok++;
                if (strlen(tok) == 0) { tok = strtok(NULL, ","); continue; }
                strncpy(g_info.files[g_info.file_count].name, tok,
                        O2RING_MAX_FILENAME - 1);
                g_info.file_count++;
                tok = strtok(NULL, ",");
            }
            free(list);
        }
    }

    g_info.valid = true;
    cJSON_Delete(root);
    free(json_str);
    xSemaphoreGive(g_cmd_mutex);

    ESP_LOGI(TAG, "Device: %s SN=%s Batt=%u%% Files=%d",
             g_info.model, g_info.serial, g_info.battery, g_info.file_count);
    for (int i = 0; i < g_info.file_count; i++)
        ESP_LOGI(TAG, "  [%d] %s", i, g_info.files[i].name);

    return ESP_OK;
}

const o2ring_device_info_t *o2ring_ble_get_info(void)
{
    return &g_info;
}

void o2ring_ble_set_auto_reconnect(bool enable)
{
    g_auto_reconnect = enable;
}

esp_err_t o2ring_ble_disconnect(void)
{
    if (!g_connected) return ESP_OK;
    return esp_ble_gattc_close(g_if, g_conn_id);
}

esp_err_t o2ring_ble_connect_and_wait(uint32_t timeout_ms)
{
    if (g_ready) return ESP_OK;

    xEventGroupClearBits(g_cmd_events, EVT_CONNECTED);
    esp_ble_gap_start_scanning(0);

    /* Poll in 500ms slices — BLE callbacks set g_ready asynchronously */
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        EventBits_t bits = xEventGroupWaitBits(g_cmd_events, EVT_CONNECTED,
                                                pdTRUE, pdTRUE,
                                                pdMS_TO_TICKS(500));
        if (bits & EVT_CONNECTED) return ESP_OK;
        if (g_ready) return ESP_OK;
        elapsed += 500;
    }

    esp_ble_gap_stop_scanning();
    return ESP_ERR_TIMEOUT;
}

esp_err_t o2ring_ble_read_sensors(void)
{
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(g_cmd_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    esp_err_t ret = send_and_wait(CMD_READ_SENSORS, 0, NULL, 0, 5000);
    if (ret != ESP_OK) {
        xSemaphoreGive(g_cmd_mutex);
        return ret;
    }

    if (g_resp_len >= 13) {
        g_live.spo2 = g_resp_buf[0];
        g_live.hr = g_resp_buf[1];
        g_live.motion = g_resp_buf[9];
        g_live.vibration = g_resp_buf[12];
        g_live.timestamp = esp_timer_get_time();
        g_live.valid = (g_live.spo2 != 0xFF && g_live.hr != 0xFF);
        ESP_LOGI(TAG, "Live: SpO2=%u HR=%u motion=%u vib=%u",
                 g_live.spo2, g_live.hr, g_live.motion, g_live.vibration);
    }

    xSemaphoreGive(g_cmd_mutex);
    return ESP_OK;
}

const o2ring_live_t *o2ring_ble_get_live(void)
{
    return &g_live;
}

esp_err_t o2ring_ble_download_file(const char *filename,
                                    uint8_t *out_buf, size_t buf_size,
                                    size_t *out_len)
{
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(g_cmd_mutex, pdMS_TO_TICKS(30000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "Downloading %s", filename);

    /* FILE_OPEN — payload is filename + null terminator */
    size_t name_len = strlen(filename) + 1;
    esp_err_t ret = send_and_wait(CMD_FILE_OPEN, 0,
                                   (const uint8_t *)filename, name_len, 10000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FILE_OPEN failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(g_cmd_mutex);
        return ret;
    }

    /* FILE_OPEN response contains file size as uint32_t LE in first 4 bytes */
    if (g_resp_len < 4) {
        ESP_LOGE(TAG, "FILE_OPEN response too short (%u bytes)", (unsigned)g_resp_len);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t file_size = g_resp_buf[0] | ((uint32_t)g_resp_buf[1] << 8) |
                          ((uint32_t)g_resp_buf[2] << 16) | ((uint32_t)g_resp_buf[3] << 24);
    ESP_LOGI(TAG, "File size: %lu bytes", (unsigned long)file_size);

    if (file_size == 0 || file_size > buf_size) {
        ESP_LOGE(TAG, "File too large or empty: %lu (buf=%u)",
                 (unsigned long)file_size, (unsigned)buf_size);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    /* FILE_READ loop — set up download state, send first block request */
    g_dl_buf = out_buf;
    g_dl_buf_size = buf_size;
    g_dl_offset = 0;
    g_dl_file_size = file_size;
    g_dl_block = 0;
    g_dl_active = true;

    xEventGroupClearBits(g_cmd_events, EVT_RESPONSE_READY);
    ret = send_cmd(CMD_FILE_READ, 0, NULL, 0);
    if (ret != ESP_OK) {
        g_dl_active = false;
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(g_cmd_mutex);
        return ret;
    }

    /* Wait for download to complete (dispatch_frame handles block sequencing) */
    EventBits_t bits = xEventGroupWaitBits(g_cmd_events, EVT_RESPONSE_READY,
                                            pdTRUE, pdTRUE,
                                            pdMS_TO_TICKS(120000));
    g_dl_active = false;

    if (!(bits & EVT_RESPONSE_READY)) {
        ESP_LOGE(TAG, "Download timeout at %lu/%lu bytes",
                 (unsigned long)g_dl_offset, (unsigned long)file_size);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_TIMEOUT;
    }

    *out_len = g_dl_offset;
    ESP_LOGI(TAG, "Downloaded %lu bytes", (unsigned long)g_dl_offset);

    /* FILE_CLOSE — wait for dedicated ACK bit */
    xEventGroupClearBits(g_cmd_events, EVT_CLOSE_ACK);
    send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
    xEventGroupWaitBits(g_cmd_events, EVT_CLOSE_ACK,
                        pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));

    xSemaphoreGive(g_cmd_mutex);
    return ESP_OK;
}

esp_err_t o2ring_ble_download_file_stream(const char *filename,
                                           o2ring_chunk_cb_t cb, void *ctx,
                                           size_t *out_len)
{
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(g_cmd_mutex, pdMS_TO_TICKS(30000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "Streaming download: %s", filename);

    size_t name_len = strlen(filename) + 1;
    esp_err_t ret = send_and_wait(CMD_FILE_OPEN, 0,
                                   (const uint8_t *)filename, name_len, 10000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FILE_OPEN failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(g_cmd_mutex);
        return ret;
    }

    if (g_resp_len < 4) {
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t file_size = g_resp_buf[0] | ((uint32_t)g_resp_buf[1] << 8) |
                          ((uint32_t)g_resp_buf[2] << 16) | ((uint32_t)g_resp_buf[3] << 24);
    ESP_LOGI(TAG, "File size: %lu bytes", (unsigned long)file_size);

    if (file_size == 0) {
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    g_dl_buf = NULL;
    g_dl_buf_size = 0;
    g_dl_cb = cb;
    g_dl_cb_ctx = ctx;
    g_dl_cb_error = false;
    g_dl_offset = 0;
    g_dl_file_size = file_size;
    g_dl_block = 0;
    g_dl_active = true;

    xEventGroupClearBits(g_cmd_events, EVT_RESPONSE_READY);
    ret = send_cmd(CMD_FILE_READ, 0, NULL, 0);
    if (ret != ESP_OK) {
        g_dl_active = false;
        g_dl_cb = NULL;
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(g_cmd_mutex);
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(g_cmd_events, EVT_RESPONSE_READY,
                                            pdTRUE, pdTRUE,
                                            pdMS_TO_TICKS(120000));
    g_dl_active = false;
    g_dl_cb = NULL;

    if (g_dl_cb_error) {
        ESP_LOGE(TAG, "Stream callback error at %lu bytes", (unsigned long)g_dl_offset);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(g_cmd_mutex);
        return ESP_FAIL;
    }

    if (!(bits & EVT_RESPONSE_READY)) {
        ESP_LOGE(TAG, "Stream timeout at %lu/%lu bytes",
                 (unsigned long)g_dl_offset, (unsigned long)file_size);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_TIMEOUT;
    }

    if (out_len) *out_len = g_dl_offset;
    ESP_LOGI(TAG, "Streamed %lu bytes", (unsigned long)g_dl_offset);

    xEventGroupClearBits(g_cmd_events, EVT_CLOSE_ACK);
    send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
    xEventGroupWaitBits(g_cmd_events, EVT_CLOSE_ACK,
                        pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));

    xSemaphoreGive(g_cmd_mutex);
    return ESP_OK;
}
