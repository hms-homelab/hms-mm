/**
 * @file o2ring_ble_nimble.c
 * @brief Wellue / Viatom O2 ring BLE GATT client, on NimBLE.
 *
 * Replaces the Bluedroid client. Bluedroid's footprint is the reason the ring
 * and a held ezShare link did not fit in the C3's heap together; NimBLE,
 * trimmed to one central connection (sdkconfig.defaults), does. The public
 * surface is o2ring_ble.h, unchanged for the scanner.
 */

#include "o2ring_ble.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_timer.h"

#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/ble.h"

#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_sm.h"
#include "host/util/util.h"

#include "cJSON.h"
#include "mbedtls/aes.h"

#include "oxyii_proto.h"

static const char *TAG = "o2ring_nimble";

/* ── UUIDs (128-bit, little-endian byte order) ── */

static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x39, 0x23, 0xcf, 0x40, 0x73, 0x16, 0x42, 0x9a,
    0x5c, 0x41, 0x7e, 0x7d, 0xc4, 0x9a, 0x83, 0x14);
static const ble_uuid128_t WRITE_UUID = BLE_UUID128_INIT(
    0xa3, 0xe1, 0x26, 0x0a, 0xee, 0x9a, 0xe9, 0xbb,
    0xb0, 0x49, 0x0b, 0xeb, 0xe7, 0xac, 0x00, 0x8b);
static const ble_uuid128_t NOTIFY_UUID = BLE_UUID128_INIT(
    0x57, 0x9a, 0x05, 0x43, 0x52, 0xcd, 0xb1, 0xa6,
    0x1a, 0x4b, 0xe7, 0xa8, 0x4a, 0x59, 0x34, 0x07);

/* SDD-001: the O2Ring-S (T8520) has none of the above. It speaks "OxyII" on
 * E8FB0001-A14B-98F9-831B-4E2941D01248, write ...0002, notify ...0003. */
static const ble_uuid128_t OXY_SVC_UUID = BLE_UUID128_INIT(
    0x48, 0x12, 0xd0, 0x41, 0x29, 0x4e, 0x1b, 0x83,
    0xf9, 0x98, 0x4b, 0xa1, 0x01, 0x00, 0xfb, 0xe8);
static const ble_uuid128_t OXY_WRITE_UUID = BLE_UUID128_INIT(
    0x48, 0x12, 0xd0, 0x41, 0x29, 0x4e, 0x1b, 0x83,
    0xf9, 0x98, 0x4b, 0xa1, 0x02, 0x00, 0xfb, 0xe8);
static const ble_uuid128_t OXY_NOTIFY_UUID = BLE_UUID128_INIT(
    0x48, 0x12, 0xd0, 0x41, 0x29, 0x4e, 0x1b, 0x83,
    0xf9, 0x98, 0x4b, 0xa1, 0x03, 0x00, 0xfb, 0xe8);

/* Which protocol the ring in hand speaks, chosen when the scan matches it. A
 * cached address in NVS is always a legacy ring: an O2Ring-S is never cached
 * across boots, because its address changes between its worn and idle
 * identities and on a factory reset. */
typedef enum { RING_LEGACY = 0, RING_OXYII } ring_proto_t;
static ring_proto_t s_proto = RING_LEGACY;
/* The match was the worn identity ("T8520_..."), which may not expose the
 * OxyII service. When discovery confirms that, that identity is passed over
 * until the stack restarts, so the idle one ("S8-AW") is found. */
static bool s_oxy_by_t8520 = false;
static bool s_oxy_skip_t8520 = false;

/* ── Viatom protocol constants ── */

#define VIATOM_REQ_HDR   0xAA
#define VIATOM_RESP_HDR  0x55
#define CMD_INFO          0x14
#define CMD_READ_SENSORS  0x17
#define CMD_FILE_OPEN     0x03
#define CMD_FILE_READ     0x04
#define CMD_FILE_CLOSE    0x05

/* ── NimBLE / connection state ── */

static uint8_t  s_own_addr_type;
static ble_addr_t s_peer_addr;
static bool     s_have_cached = false;
static bool     s_connecting_cached = false;
static bool     s_connecting = false;
static bool     s_connected = false;
static bool     s_scanning = false;
static bool     s_ready = false;
static bool     s_stack_running = false;
static bool     s_stopping = false;
static bool     s_auto_reconnect = true;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_write_handle, s_notify_handle, s_cccd_handle;

static bool     s_service_found = false;

/* ── Synchronization for blocking commands ── */

static SemaphoreHandle_t s_cmd_mutex;
static EventGroupHandle_t s_cmd_events;
#define EVT_RESPONSE_READY  (1 << 0)
#define EVT_CONNECTED       (1 << 1)
#define EVT_CLOSE_ACK       (1 << 2)
#define EVT_DISCONNECTED    (1 << 3)   /* deinit waits for this before the host stop */

/* Cached live reading */
static o2ring_live_t s_live = {0};

/* Response buffer — filled by notification handler, read by command caller */
#define RESP_BUF_SIZE  4096
static uint8_t s_resp_buf[RESP_BUF_SIZE];
static size_t  s_resp_len;

/* Frame reassembly buffer */
#define REASM_BUF_SIZE 4096
static uint8_t s_reasm[REASM_BUF_SIZE];
static size_t  s_reasm_len;

/* File download state */
static uint8_t *s_dl_buf;
static size_t   s_dl_buf_size;
static size_t   s_dl_offset;
static uint32_t s_dl_file_size;
static uint16_t s_dl_block;
static bool     s_dl_active;
static bool     s_dl_cb_error;
static o2ring_chunk_cb_t s_dl_cb;
static void    *s_dl_cb_ctx;

/* Cached device info */
static o2ring_device_info_t s_info;

/* OxyII request/reply state. A reply carries the request's opcode; the caller
 * waits for that opcode and anything else is dropped. s_oxy_session is the
 * per-connection handshake (auth, setup, time) and is cleared on every link. */
static uint8_t           s_oxy_seq;
static volatile int      s_oxy_wait_op = -1;
static bool              s_oxy_session;
static oxyii_info_t      s_oxy_info;

/* Set when the ring answered auth with a session key: every payload after it
 * is AES-128-ECB (oxyii_proto.h). Per connection, like s_oxy_session. */
static bool                s_oxy_encrypted;
static mbedtls_aes_context s_oxy_aes_enc;
static mbedtls_aes_context s_oxy_aes_dec;

/* Notification flatten buffer (host callback context). */
#define NOTIFY_FLAT_BUF_SIZE 1024
static uint8_t s_notify_flat_buf[NOTIFY_FLAT_BUF_SIZE];

/* ── Cached ring BLE address (NVS) ── */
#define O2RING_NVS_NS    "o2ring"

static void o2ring_load_cached_addr(void)
{
    nvs_handle_t h;
    if (nvs_open(O2RING_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t sz = sizeof(s_peer_addr.val);
    uint8_t atype = 0;
    if (nvs_get_blob(h, "mac", s_peer_addr.val, &sz) == ESP_OK &&
        sz == sizeof(s_peer_addr.val) &&
        nvs_get_u8(h, "atype", &atype) == ESP_OK) {
        s_peer_addr.type = atype;
        s_have_cached = true;
        ESP_LOGI(TAG, "Loaded cached O2Ring %02x:%02x:%02x:%02x:%02x:%02x (type %u)",
                 s_peer_addr.val[0], s_peer_addr.val[1], s_peer_addr.val[2],
                 s_peer_addr.val[3], s_peer_addr.val[4], s_peer_addr.val[5], atype);
    }
    nvs_close(h);
}

static void o2ring_save_cached_addr(void)
{
    nvs_handle_t h;
    if (nvs_open(O2RING_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "mac", s_peer_addr.val, sizeof(s_peer_addr.val));
    nvs_set_u8(h, "atype", (uint8_t)s_peer_addr.type);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Cached O2Ring address to NVS");
}

static void o2ring_clear_cached_addr(void)
{
    s_have_cached = false;
    nvs_handle_t h;
    if (nvs_open(O2RING_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "mac");
    nvs_erase_key(h, "atype");
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "Cleared cached O2Ring address");
}

/* When the ring is simply off, a direct connect to the cached address never
 * completes and no event arrives, so without this the same dead address would
 * be retried forever. Forgetting on the FIRST failure is too aggressive: one
 * transient miss throws away a working address. Hence the counter. */
#define CACHED_FORGET_AFTER 3
static uint8_t s_cached_fail_count = 0;

bool o2ring_ble_forget_cached_if_direct(void)
{
    if (!s_connecting_cached) return false;   /* scan-based timeout: ring wasn't
                                                 advertising, cache is not at fault */
    s_connecting_cached = false;
    if (++s_cached_fail_count < CACHED_FORGET_AFTER) {
        ESP_LOGW(TAG, "cached-addr connect failed (%u/%u) — keeping the address",
                 s_cached_fail_count, CACHED_FORGET_AFTER);
        return false;
    }
    s_cached_fail_count = 0;
    o2ring_clear_cached_addr();
    return true;
}

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
    if (!s_ready || s_write_handle == 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return ESP_ERR_INVALID_STATE;

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
    if (payload && payload_len > 0) memcpy(frame + 7, payload, payload_len);
    frame[frame_len - 1] = crc8(frame, frame_len - 1);

    size_t sent = 0;
    esp_err_t ret = ESP_OK;
    while (sent < frame_len) {
        size_t chunk = frame_len - sent;
        if (chunk > 20) chunk = 20;
        ret = ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle,
                                          frame + sent, chunk);
        if (ret != ESP_OK) break;
        sent += chunk;
        if (sent < frame_len) vTaskDelay(pdMS_TO_TICKS(20));
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

    if (s_dl_active) {
        const uint8_t *data = frame + 7;

        if (s_dl_cb) {
            if (!s_dl_cb(data, payload_len, s_dl_offset, s_dl_cb_ctx)) {
                s_dl_cb_error = true;
                xEventGroupSetBits(s_cmd_events, EVT_RESPONSE_READY);
                return;
            }
        } else if (s_dl_buf) {
            size_t to_copy = payload_len;
            if (s_dl_offset + to_copy > s_dl_buf_size)
                to_copy = s_dl_buf_size - s_dl_offset;
            if (to_copy > 0) memcpy(s_dl_buf + s_dl_offset, data, to_copy);
        }

        s_dl_offset += payload_len;
        if (s_dl_offset >= s_dl_file_size) {
            xEventGroupSetBits(s_cmd_events, EVT_RESPONSE_READY);
        } else {
            s_dl_block++;
            send_cmd(CMD_FILE_READ, s_dl_block, NULL, 0);
        }
        return;
    }

    if (payload_len <= RESP_BUF_SIZE && 7 + payload_len <= len) {
        memcpy(s_resp_buf, frame + 7, payload_len);
        s_resp_len = payload_len;
    } else {
        ESP_LOGW(TAG, "Frame payload exceeds buffer (pl=%u buf=%d frame=%zu)",
                 payload_len, RESP_BUF_SIZE, len);
        s_resp_len = 0;
    }

    uint8_t resp_cmd = frame[1];
    EventBits_t bits_to_set = EVT_RESPONSE_READY;
    if (resp_cmd == CMD_FILE_CLOSE) bits_to_set |= EVT_CLOSE_ACK;
    xEventGroupSetBits(s_cmd_events, bits_to_set);
}

/* OxyII replies: the one the caller waits for lands in s_resp_buf. Runs in the
 * NimBLE host task, so it never blocks: downloads are driven from the caller's
 * task, one F3 per reply, not chained from here as the legacy path does. */
static void oxy_process_notification(void)
{
    for (;;) {
        size_t skip = 0, flen = 0;
        bool whole = oxyii_find_frame(s_reasm, s_reasm_len, &skip, &flen);
        if (skip) {
            memmove(s_reasm, s_reasm + skip, s_reasm_len - skip);
            s_reasm_len -= skip;
        }
        if (!whole) return;

        uint8_t  op   = oxyii_frame_op(s_reasm);
        uint16_t plen = oxyii_frame_len(s_reasm);
        if ((int)op == s_oxy_wait_op && plen <= RESP_BUF_SIZE) {
            memcpy(s_resp_buf, oxyii_frame_payload(s_reasm), plen);
            s_resp_len = plen;
            s_oxy_wait_op = -1;
            xEventGroupSetBits(s_cmd_events, EVT_RESPONSE_READY);
        } else {
            ESP_LOGD(TAG, "oxyii: unawaited reply op=0x%02x len=%u", op, plen);
        }
        memmove(s_reasm, s_reasm + flen, s_reasm_len - flen);
        s_reasm_len -= flen;
    }
}

static void process_notification(const uint8_t *data, size_t len)
{
    if (s_reasm_len + len > REASM_BUF_SIZE) s_reasm_len = 0;
    memcpy(s_reasm + s_reasm_len, data, len);
    s_reasm_len += len;

    if (s_proto == RING_OXYII) {
        oxy_process_notification();
        return;
    }

    while (s_reasm_len > 0) {
        while (s_reasm_len > 0 &&
               s_reasm[0] != VIATOM_REQ_HDR && s_reasm[0] != VIATOM_RESP_HDR) {
            memmove(s_reasm, s_reasm + 1, --s_reasm_len);
        }
        if (s_reasm_len < 7) return;

        if ((s_reasm[1] ^ 0xFF) != s_reasm[2]) {
            memmove(s_reasm, s_reasm + 1, --s_reasm_len);
            continue;
        }

        uint16_t plen = s_reasm[5] | ((uint16_t)s_reasm[6] << 8);
        size_t total = 7 + plen + 1;
        if (s_reasm_len < total) return;

        uint8_t want = s_reasm[total - 1];
        uint8_t got  = crc8(s_reasm, total - 1);
        if (want != got) {
            ESP_LOGW(TAG, "CRC fail: want=0x%02x got=0x%02x", want, got);
            memmove(s_reasm, s_reasm + 1, --s_reasm_len);
            continue;
        }

        dispatch_frame(s_reasm, total);

        if (s_reasm_len > total) memmove(s_reasm, s_reasm + total, s_reasm_len - total);
        s_reasm_len -= total;
    }
}

/* ── Send command and wait for response ── */

static esp_err_t send_and_wait(uint8_t cmd, uint16_t block,
                               const uint8_t *payload, uint16_t payload_len,
                               uint32_t timeout_ms)
{
    xEventGroupClearBits(s_cmd_events, EVT_RESPONSE_READY);
    s_resp_len = 0;

    esp_err_t ret = send_cmd(cmd, block, payload, payload_len);
    if (ret != ESP_OK) return ret;

    EventBits_t bits = xEventGroupWaitBits(s_cmd_events, EVT_RESPONSE_READY,
                                           pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EVT_RESPONSE_READY)) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

/* ── OxyII (O2Ring-S) requests, SDD-001 ── */

static void oxy_aes_enc_block(void *ctx, const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_crypt_ecb((mbedtls_aes_context *)ctx, MBEDTLS_AES_ENCRYPT, in, out);
}

static void oxy_aes_dec_block(void *ctx, const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_crypt_ecb((mbedtls_aes_context *)ctx, MBEDTLS_AES_DECRYPT, in, out);
}

/* One frame, in as few writes as the negotiated MTU allows: with the 517 the
 * ring needs, every request is a single write. In an encrypted session a
 * non-empty payload goes as ciphertext; auth never does. */
static esp_err_t oxy_send(uint8_t op, const uint8_t *payload, uint16_t len)
{
    if (!s_ready || s_write_handle == 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return ESP_ERR_INVALID_STATE;
    uint8_t cipher[64];
    if (s_oxy_encrypted && len > 0 && op != OXYII_OP_AUTH) {
        size_t clen = oxyii_encrypt(oxy_aes_enc_block, &s_oxy_aes_enc, payload, len,
                                    cipher, sizeof(cipher));
        if (clen == 0) return ESP_ERR_INVALID_SIZE;
        payload = cipher;
        len = (uint16_t)clen;
    }
    size_t frame_len = oxyii_encode(op, s_oxy_seq++, payload, len,
                                    s_cmd_frame, sizeof(s_cmd_frame));
    if (frame_len == 0) return ESP_ERR_INVALID_SIZE;

    uint16_t mtu = ble_att_mtu(s_conn_handle);
    size_t max = mtu > 3 ? (size_t)mtu - 3 : 20;
    for (size_t sent = 0; sent < frame_len; ) {
        size_t chunk = frame_len - sent;
        if (chunk > max) chunk = max;
        int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle,
                                             s_cmd_frame + sent, chunk);
        if (rc != 0) return ESP_FAIL;
        sent += chunk;
        if (sent < frame_len) vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

/* Sends op and waits for the reply with the same opcode. A dropped link ends
 * the wait at once rather than at the timeout. */
static esp_err_t oxy_request(uint8_t op, const uint8_t *payload, uint16_t len,
                             uint32_t timeout_ms)
{
    xEventGroupClearBits(s_cmd_events, EVT_RESPONSE_READY | EVT_DISCONNECTED);
    s_resp_len = 0;
    s_oxy_wait_op = op;
    esp_err_t ret = oxy_send(op, payload, len);
    if (ret != ESP_OK) {
        s_oxy_wait_op = -1;
        return ret;
    }
    EventBits_t bits = xEventGroupWaitBits(s_cmd_events,
                                           EVT_RESPONSE_READY | EVT_DISCONNECTED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    xEventGroupClearBits(s_cmd_events, EVT_RESPONSE_READY);
    s_oxy_wait_op = -1;
    if (!(bits & EVT_RESPONSE_READY))
        return (bits & EVT_DISCONNECTED) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;

    /* Decrypted here, in the caller's task, not in the host task that copied it. */
    if (s_oxy_encrypted && s_resp_len > 0 && oxyii_reply_encrypted(op)) {
        int n = oxyii_decrypt(oxy_aes_dec_block, &s_oxy_aes_dec, s_resp_buf, s_resp_len);
        if (n < 0) {
            ESP_LOGW(TAG, "oxyii: reply to 0x%02x (%u bytes) did not decrypt",
                     op, (unsigned)s_resp_len);
            s_resp_len = 0;
            return ESP_ERR_INVALID_RESPONSE;
        }
        s_resp_len = (size_t)n;
    }
    return ESP_OK;
}

/* The ring answered auth: is it a session key? Sets up both AES contexts when it is. */
static void oxy_take_auth_reply(void)
{
    uint8_t key[OXYII_KEY_LEN];
    if (!oxyii_parse_auth_reply(s_resp_buf, s_resp_len, key)) {
        ESP_LOGI(TAG, "oxyii: auth answered with %u bytes, no key: plaintext session",
                 (unsigned)s_resp_len);
        return;
    }
    mbedtls_aes_free(&s_oxy_aes_enc);
    mbedtls_aes_free(&s_oxy_aes_dec);
    mbedtls_aes_init(&s_oxy_aes_enc);
    mbedtls_aes_init(&s_oxy_aes_dec);
    if (mbedtls_aes_setkey_enc(&s_oxy_aes_enc, key, 128) != 0 ||
        mbedtls_aes_setkey_dec(&s_oxy_aes_dec, key, 128) != 0) {
        ESP_LOGE(TAG, "oxyii: session key rejected by AES setup");
        return;
    }
    s_oxy_encrypted = true;
    ESP_LOGI(TAG, "oxyii: session key received: AES-128 session");
}

/* The per-connection handshake, run once on each link before anything else, in
 * the vendor SDK's order: auth 0xFF, setup 0x10, GET_INFO, set time 0xC0,
 * GET_CONFIG 0x00. File opens are silently refused without it.
 *
 * Auth waits a second for an answer, as the vendor apps do. Older rings never
 * give one and the session stays plaintext; newer ones answer with a session
 * key and everything after is encrypted (oxyii_proto.h). The timestamp in the
 * auth payload is a nonce the ring does not check.
 *
 * The miner has no wall clock, so 0xC0 writes back the time the ring itself
 * reported in GET_INFO: the step the ring wants is done and its clock, and so
 * its file names, stay as the owner's app set them. Called with s_cmd_mutex
 * held. */
static esp_err_t oxy_ensure_session(void)
{
    if (s_oxy_session) return ESP_OK;
    s_oxy_encrypted = false;

    uint8_t auth[OXYII_AUTH_LEN];
    oxyii_auth_payload(0, auth);
    esp_err_t ret = oxy_request(OXYII_OP_AUTH, auth, sizeof(auth), 1000);
    if (ret == ESP_OK) {
        oxy_take_auth_reply();
    } else if (ret != ESP_ERR_TIMEOUT) {
        return ret;
    }

    /* Required on a plaintext ring; on an encrypted one the vendor apps only
     * log its failure, and so does this. */
    const uint8_t zero = 0x00;
    ret = oxy_request(OXYII_OP_SETUP, &zero, 1, 3000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "oxyii: setup 0x10 failed: %s", esp_err_to_name(ret));
        if (!s_oxy_encrypted) return ret;
    }

    ret = oxy_request(OXYII_OP_GET_INFO, NULL, 0, 5000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "oxyii: GET_INFO failed: %s", esp_err_to_name(ret));
        return ret;
    }
    oxyii_parse_info(s_resp_buf, s_resp_len, &s_oxy_info);
    ESP_LOGI(TAG, "oxyii: O2Ring-S SN=%s fw=%s clock=%04u-%02u-%02u %02u:%02u:%02u%s%s",
             s_oxy_info.serial, s_oxy_info.firmware, s_oxy_info.clock.year,
             s_oxy_info.clock.month, s_oxy_info.clock.day, s_oxy_info.clock.hour,
             s_oxy_info.clock.minute, s_oxy_info.clock.second,
             s_oxy_info.clock_valid ? "" : " (NOT SET)",
             s_oxy_encrypted ? " (encrypted)" : "");

    if (s_oxy_info.clock_valid) {
        uint8_t t[OXYII_TIME_LEN];
        oxyii_time_payload(&s_oxy_info.clock, t);
        ret = oxy_request(OXYII_OP_SET_TIME, t, sizeof(t), 3000);
        if (ret != ESP_OK) ESP_LOGW(TAG, "oxyii: set time: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGW(TAG, "oxyii: the ring's clock is not set; file reads may be refused "
                      "until its app has set it once");
    }

    ret = oxy_request(OXYII_OP_GET_CONFIG, NULL, 0, 3000);
    if (ret != ESP_OK) ESP_LOGW(TAG, "oxyii: GET_CONFIG: %s", esp_err_to_name(ret));

    s_oxy_session = true;
    return ESP_OK;
}

/* 0xF4 clears a handle the ring's own overnight recording left open, without
 * which 0xF1 hangs (the "F1 wedge"). A no-op when nothing is open, and some
 * firmware does not answer it then, so a timeout is not a failure. */
static void oxy_close_file(void)
{
    esp_err_t ret = oxy_request(OXYII_OP_FILE_END, NULL, 0, 2000);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT)
        ESP_LOGW(TAG, "oxyii: close: %s", esp_err_to_name(ret));
}

/* ── Helpers ── */

static bool uuid128_eq(const ble_uuid_t *u, const ble_uuid128_t *target)
{
    return ble_uuid_cmp(u, &target->u) == 0;
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg);
static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg);
static int dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                       void *arg);
static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg);
static void start_connection_attempt(void);

/* ── Which advertiser is a ring ──
 *
 * The whole Viatom/Wellue oximeter line (O2Ring, Checkme O2 / O2 Ultra, SleepU,
 * PO1-PO4, ...) shares one GATT profile, but each model advertises a different
 * name. Match any known name substring, case-insensitive; failing that, the
 * advertised 128-bit service UUID, which is constant across the family (some
 * models drop their name after they have been paired once). */
static const char *KNOWN_NAME_SUBSTRINGS[] = {
    "o2ring", "checkme", "checko2", "viatom", "wellue", "sleepu", "oxyring",
    /* The Checkme O2 Ultra advertises as "Band-WU XXXX", with only the 16-bit
     * Heart Rate service (180D) in its adv data, so the UUID fallback below
     * never sees it: the name is the only hook. "band-wu", not "band": a bare
     * "band" matches any fitness tracker in range, and we would cache a
     * stranger's address in NVS and then fail discovery against it. */
    "band-wu",
};

static bool name_matches_known_device(const uint8_t *name, size_t name_len)
{
    if (!name || name_len == 0) return false;
    char lower[33] = {0};
    size_t n = name_len < sizeof(lower) - 1 ? name_len : sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++) {
        char c = (char)name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    for (size_t i = 0; i < sizeof(KNOWN_NAME_SUBSTRINGS) / sizeof(KNOWN_NAME_SUBSTRINGS[0]); i++) {
        if (strstr(lower, KNOWN_NAME_SUBSTRINGS[i])) return true;
    }
    return false;
}

static bool adv_advertises_svc_uuid(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids128; i++) {
        if (ble_uuid_cmp(&f->uuids128[i].u, &SVC_UUID.u) == 0) return true;
    }
    return false;
}

/* SDD-001: an O2Ring-S, by its name or the OxyII service. *by_t8520 says the
 * match was only its worn-mode name. */
static bool adv_is_oxyii(const struct ble_hs_adv_fields *f, const uint8_t *name,
                         size_t name_len, bool *by_t8520)
{
    char lower[33] = {0};
    size_t n = 0;
    if (name) n = name_len < sizeof(lower) - 1 ? name_len : sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++) {
        char c = (char)name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    /* By its name or its OxyII service only. NOT by manufacturer ID 0xF34E:
     * the original O2Ring advertises it too, and matching on it took that
     * ring down this path. */
    bool svc = false;
    for (int i = 0; i < f->num_uuids128; i++)
        if (ble_uuid_cmp(&f->uuids128[i].u, &OXY_SVC_UUID.u) == 0) svc = true;
    bool named = n > 0 && oxyii_name_matches(lower);
    if (by_t8520) *by_t8520 = !svc && strncmp(lower, "t8520", 5) == 0;
    return svc || named;
}

static void fail_and_rescan(void)
{
    if (s_stopping) return;
    bool was_connected = s_connected;
    uint16_t conn_handle = s_conn_handle;
    s_connecting_cached = false;
    s_connecting = false;
    s_connected = false;
    s_ready = false;
    s_service_found = false;
    s_write_handle = 0;
    s_notify_handle = 0;
    s_cccd_handle = 0;
    s_svc_start = s_svc_end = 0;
    s_reasm_len = 0;
    s_dl_active = false;
    s_dl_buf = NULL;
    s_dl_cb = NULL;
    s_dl_cb_ctx = NULL;
    s_dl_buf_size = 0;
    s_dl_offset = 0;
    s_resp_len = 0;
    if (was_connected && conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    if (s_auto_reconnect) start_connection_attempt();
}

/* ── GAP / GATT callbacks ── */

static int gap_event(struct ble_gap_event *event, void *arg);

static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (error->status != 0) {
        ESP_LOGE(TAG, "CCCD write failed: %u (attr=0x%04x)", error->status, attr->handle);
        fail_and_rescan();
        return 0;
    }

    ESP_LOGI(TAG, "Notifications enabled — ready for commands");

    /* Slave latency on the idle link. The ring negotiates itvl 6-24 / latency 0,
     * so it wakes every connection event even when we are asking it nothing —
     * airtime spent competing with the ezShare association for no benefit.
     * Latency lets it skip events while the link stays up, which is the point of
     * holding the link at all.
     *
     * supervision_timeout must exceed (1 + latency) * itvl_max * 2. With
     * itvl_max=80 (100 ms) and latency=4 that floor is 1000 ms; 400 units
     * (4000 ms) leaves margin for a missed window under WiFi contention. */
    struct ble_gap_upd_params idle = {
        .itvl_min            = 40,   /* 50 ms  */
        .itvl_max            = 80,   /* 100 ms */
        .latency             = 4,
        .supervision_timeout = 400,  /* 4000 ms */
        .min_ce_len          = 0,
        .max_ce_len          = 0,
    };
    int urc = ble_gap_update_params(conn_handle, &idle);
    if (urc != 0)
        ESP_LOGW(TAG, "conn param update rejected: %d (keeping negotiated params)", urc);

    s_ready = true;
    xEventGroupSetBits(s_cmd_events, EVT_CONNECTED);
    return 0;
}

static int dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                       void *arg)
{
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;

    switch (error->status) {
    case 0:
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            s_cccd_handle = dsc->handle;
            ESP_LOGI(TAG, "CCCD handle: 0x%04x", s_cccd_handle);
        }
        return 0;

    case BLE_HS_EDONE:
        if (!s_cccd_handle) {
            ESP_LOGE(TAG, "CCCD not found");
            fail_and_rescan();
            return 0;
        }
        {
            uint16_t en = 0x0001;
            int rc = ble_gattc_write_flat(s_conn_handle, s_cccd_handle,
                                          &en, sizeof(en), cccd_write_cb, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "CCCD subscribe write failed: %d", rc);
                fail_and_rescan();
            } else {
                ESP_LOGI(TAG, "Enabling CCCD 0x%04x", s_cccd_handle);
            }
        }
        return 0;

    default:
        ESP_LOGE(TAG, "Descriptor discovery failed: %u", error->status);
        fail_and_rescan();
        return 0;
    }
}

static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    (void)arg;

    switch (error->status) {
    case 0:
        if (uuid128_eq(&chr->uuid.u, s_proto == RING_OXYII ? &OXY_WRITE_UUID : &WRITE_UUID)) {
            s_write_handle = chr->val_handle;
            ESP_LOGI(TAG, "Write handle: 0x%04x", s_write_handle);
        }
        if (uuid128_eq(&chr->uuid.u, s_proto == RING_OXYII ? &OXY_NOTIFY_UUID : &NOTIFY_UUID)) {
            s_notify_handle = chr->val_handle;
            ESP_LOGI(TAG, "Notify handle: 0x%04x", s_notify_handle);
        }
        return 0;

    case BLE_HS_EDONE:
        if (!s_write_handle || !s_notify_handle) {
            ESP_LOGE(TAG, "Required characteristic not found");
            fail_and_rescan();
            return 0;
        }
        return ble_gattc_disc_all_dscs(s_conn_handle, s_notify_handle, s_svc_end,
                                       dsc_disc_cb, NULL);

    default:
        ESP_LOGE(TAG, "Characteristic discovery failed: %u", error->status);
        fail_and_rescan();
        return 0;
    }
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    (void)conn_handle;
    (void)arg;

    switch (error->status) {
    case 0:
        if (service && uuid128_eq(&service->uuid.u,
                                  s_proto == RING_OXYII ? &OXY_SVC_UUID : &SVC_UUID)) {
            s_svc_start = service->start_handle;
            s_svc_end = service->end_handle;
            s_service_found = true;
            ESP_LOGI(TAG, "%s service 0x%04x-0x%04x",
                     s_proto == RING_OXYII ? "OxyII" : "Viatom", s_svc_start, s_svc_end);
        }
        return 0;

    case BLE_HS_EDONE:
        if (!s_service_found) {
            ESP_LOGE(TAG, "%s service not found", s_proto == RING_OXYII ? "OxyII" : "Viatom");
            if (s_proto == RING_OXYII && s_oxy_by_t8520) {
                /* The worn identity without the service: pass it over and scan
                 * for the idle identity instead of reconnecting to this
                 * address forever. */
                ESP_LOGW(TAG, "oxyii: T8520 identity has no OxyII service; "
                              "waiting for its S8-AW identity");
                s_oxy_skip_t8520 = true;
                s_have_cached = false;
            }
            fail_and_rescan();
            return 0;
        }
        return ble_gattc_disc_all_chrs(s_conn_handle, s_svc_start, s_svc_end,
                                       chr_disc_cb, NULL);

    default:
        ESP_LOGE(TAG, "Service discovery failed: %u", error->status);
        fail_and_rescan();
        return 0;
    }
}

static void start_connection_attempt(void)
{
    if (s_stopping || !s_stack_running || s_connected || s_scanning || s_connecting) return;

    if (s_have_cached) {
        s_connecting_cached = true;
        s_connecting = true;
        ESP_LOGI(TAG, "Cached addr — connecting directly to "
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 s_peer_addr.val[0], s_peer_addr.val[1], s_peer_addr.val[2],
                 s_peer_addr.val[3], s_peer_addr.val[4], s_peer_addr.val[5]);
        int rc = ble_gap_connect(s_own_addr_type, &s_peer_addr, 15000, NULL,
                                 gap_event, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Direct connect failed: %d", rc);
            s_connecting_cached = false;
            s_connecting = false;
            o2ring_clear_cached_addr();
            s_have_cached = false;
            start_connection_attempt();
        }
        return;
    }

    struct ble_gap_disc_params scan_params = {0};
    scan_params.itvl = 0x80;
    scan_params.window = 0x40;
    scan_params.filter_policy = 0;
    scan_params.limited = 0;
    scan_params.passive = 0;
    scan_params.filter_duplicates = 1;
    scan_params.disable_observer_mode = 0;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &scan_params,
                          gap_event, NULL);
    if (rc == 0) {
        s_scanning = true;
        ESP_LOGI(TAG, "Scanning for O2 Ring...");
    } else {
        ESP_LOGE(TAG, "Scan start failed: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (event->disc.length_data == 0) break;

        {
            struct ble_hs_adv_fields fields;
            memset(&fields, 0, sizeof(fields));
            if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                        event->disc.length_data) == 0) {
                char namebuf[33] = {0};
                if (fields.name && fields.name_len > 0) {
                    size_t copy_len = fields.name_len;
                    if (copy_len > sizeof(namebuf) - 1) copy_len = sizeof(namebuf) - 1;
                    memcpy(namebuf, fields.name, copy_len);
                }
                bool match = false;
                bool by_t8520 = false;
                ring_proto_t proto = RING_LEGACY;
                if (adv_is_oxyii(&fields, fields.name, fields.name_len, &by_t8520)) {
                    if (by_t8520 && s_oxy_skip_t8520) {
                        /* passed over: its worn identity had no OxyII service */
                    } else {
                        ESP_LOGI(TAG, "Found O2Ring-S (OxyII): %s RSSI=%d",
                                 namebuf[0] ? namebuf : "<none>", event->disc.rssi);
                        match = true;
                        proto = RING_OXYII;
                    }
                } else if (name_matches_known_device(fields.name, fields.name_len)) {
                    ESP_LOGI(TAG, "Found by name: %s RSSI=%d", namebuf, event->disc.rssi);
                    match = true;
                } else if (adv_advertises_svc_uuid(&fields)) {
                    ESP_LOGI(TAG, "Found by service UUID (name=%s) RSSI=%d",
                             namebuf[0] ? namebuf : "<none>", event->disc.rssi);
                    match = true;
                }
                if (match) {
                    if (s_scanning) {
                        ble_gap_disc_cancel();
                        s_scanning = false;
                    }
                    s_peer_addr = event->disc.addr;
                    s_have_cached = true;
                    s_connecting_cached = false;
                    s_proto = proto;
                    s_oxy_by_t8520 = by_t8520;
                    /* Only a legacy ring's address is worth keeping across
                     * boots; an O2Ring-S is found by scan every time. */
                    if (proto == RING_LEGACY) o2ring_save_cached_addr();
                    ESP_LOGI(TAG, "Connecting to %02x:%02x:%02x:%02x:%02x:%02x",
                             s_peer_addr.val[0], s_peer_addr.val[1], s_peer_addr.val[2],
                             s_peer_addr.val[3], s_peer_addr.val[4], s_peer_addr.val[5]);
                    s_connecting = true;
                    int rc = ble_gap_connect(s_own_addr_type, &s_peer_addr, 15000, NULL,
                                             gap_event, NULL);
                    if (rc != 0) {
                        ESP_LOGW(TAG, "Connect after scan failed: %d", rc);
                        s_connecting_cached = false;
                        s_connecting = false;
                        if (s_auto_reconnect && !s_stopping) start_connection_attempt();
                    }
                }
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        if (!s_connected && !s_connecting && !s_stopping && s_auto_reconnect) {
            start_connection_attempt();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected");
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            s_connecting = false;
            s_connecting_cached = false;
            s_cached_fail_count = 0;
            s_ready = false;
            s_reasm_len = 0;
            s_service_found = false;
            s_write_handle = 0;
            s_notify_handle = 0;
            s_cccd_handle = 0;
            s_oxy_session = false;

            ble_att_set_preferred_mtu(BLE_ATT_MTU_MAX);
            ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
        } else {
            ESP_LOGE(TAG, "Connect failed: %d", event->connect.status);
            s_connecting = false;
            if (s_connecting_cached) {
                s_connecting_cached = false;
                o2ring_clear_cached_addr();
            }
            s_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (!s_stopping && s_auto_reconnect) start_connection_attempt();
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == s_conn_handle) {
            ESP_LOGI(TAG, "MTU updated: %u", event->mtu.value);
            /* An O2Ring-S refuses file opens below 517. Logged, not refused
             * here: a ring that reads files at less says so by working. */
            if (s_proto == RING_OXYII && event->mtu.value < 517)
                ESP_LOGW(TAG, "oxyii: MTU %u is under the 517 file reads need",
                         event->mtu.value);
            return ble_gattc_disc_svc_by_uuid(s_conn_handle,
                                              s_proto == RING_OXYII ? &OXY_SVC_UUID.u
                                                                    : &SVC_UUID.u,
                                              svc_disc_cb, NULL);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.om) {
            uint16_t n = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (n > sizeof(s_notify_flat_buf)) n = sizeof(s_notify_flat_buf);
            if (ble_hs_mbuf_to_flat(event->notify_rx.om, s_notify_flat_buf, n, NULL) == 0) {
                process_notification(s_notify_flat_buf, n);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Disconnected (reason=%d)", event->disconnect.reason);
        if (s_cmd_events) xEventGroupSetBits(s_cmd_events, EVT_DISCONNECTED);
        s_connected = false;
        s_ready = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_service_found = false;
        s_write_handle = 0;
        s_notify_handle = 0;
        s_cccd_handle = 0;
        s_svc_start = s_svc_end = 0;
        s_reasm_len = 0;
        s_dl_active = false;
        s_dl_buf = NULL;
        s_dl_cb = NULL;
        s_dl_cb_ctx = NULL;
        s_dl_buf_size = 0;
        s_dl_offset = 0;
        s_resp_len = 0;
        s_oxy_session = false;
        s_oxy_wait_op = -1;
        s_connecting = false;
        if (!s_stopping && s_auto_reconnect && !s_connecting)
            start_connection_attempt();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
    case BLE_GAP_EVENT_NOTIFY_TX:
    case BLE_GAP_EVENT_CONN_UPDATE:
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_REPEAT_PAIRING:
    default:
        return 0;
    }

    return 0;
}

static void o2ring_nimble_host_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void o2ring_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "NimBLE sync complete");
    start_connection_attempt();
}

static void o2ring_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset: %d", reason);
}

/* ── OxyII operations behind the public API (SDD-001). Called with s_cmd_mutex held. ── */

static esp_err_t oxy_refresh_info(void)
{
    s_info.valid = false;
    esp_err_t ret = oxy_ensure_session();
    if (ret != ESP_OK) return ret;

    oxy_close_file();   /* the F1 wedge */
    ret = oxy_request(OXYII_OP_FILE_LIST, NULL, 0, 5000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "oxyii: file list failed: %s", esp_err_to_name(ret));
        return ret;
    }
    memset(s_info.files, 0, sizeof(s_info.files));
    int total = 0;
    s_info.file_count = oxyii_parse_file_list(s_resp_buf, s_resp_len, s_info.files[0].name,
                                              sizeof(s_info.files[0]), O2RING_MAX_FILES,
                                              &total);

    /* GET_BATTERY: its byte [1] is the percentage. */
    if (oxy_request(OXYII_OP_GET_BATTERY, NULL, 0, 3000) == ESP_OK && s_resp_len >= 2)
        s_info.battery = s_resp_buf[1];

    strncpy(s_info.model, "O2Ring-S", sizeof(s_info.model) - 1);
    strncpy(s_info.serial, s_oxy_info.serial, sizeof(s_info.serial) - 1);
    s_info.valid = true;

    ESP_LOGI(TAG, "Device: %s SN=%s Batt=%u%% Files=%d (of %d on the ring)",
             s_info.model, s_info.serial, s_info.battery, s_info.file_count, total);
    for (int i = 0; i < s_info.file_count; i++)
        ESP_LOGI(TAG, "  [%d] %s", i, s_info.files[i].name);
    return ESP_OK;
}

static esp_err_t oxy_read_sensors(void)
{
    esp_err_t ret = oxy_ensure_session();
    if (ret != ESP_OK) return ret;
    ret = oxy_request(OXYII_OP_LIVE, NULL, 0, 5000);
    if (ret != ESP_OK) return ret;

    oxyii_live_t l;
    if (!oxyii_parse_live(s_resp_buf, s_resp_len, &l)) {
        ESP_LOGW(TAG, "oxyii: live reply too short (%u bytes)", (unsigned)s_resp_len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_live.spo2 = l.reading ? l.spo2 : 0xFF;
    s_live.hr = l.reading ? l.hr : 0xFF;
    s_live.motion = l.motion;
    s_live.vibration = 0;
    s_live.timestamp = esp_timer_get_time();
    s_live.valid = l.reading;
    ESP_LOGI(TAG, "Live: SpO2=%u HR=%u PI=%u.%u motion=%u sensor=%u run=%u batt=%u%%",
             l.spo2, l.hr, l.pi_x10 / 10, l.pi_x10 % 10, l.motion, l.state, l.run, l.battery);
    return ESP_OK;
}

/* Firmware 2D010003 caps what one connection may transfer, so a pull that
 * stalls part-way reconnects and carries on at its offset. Each resume is
 * bounded by the 15 s connect, inside the 30 s the mule waits for the next
 * chunk. */
#define OXY_MAX_RESUMES 3

static esp_err_t oxy_download_stream(const char *filename, o2ring_size_cb_t size_cb,
                                     o2ring_chunk_cb_t cb, void *ctx, size_t *out_len)
{
    uint8_t start[OXYII_FILE_START_LEN];
    if (!oxyii_file_start_payload(filename, start)) {
        ESP_LOGE(TAG, "oxyii: %s is not an O2Ring-S file name", filename);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Streaming download: %s", filename);

    uint32_t size = 0, offset = 0;
    bool sized = false;
    int resumes = 0;
    esp_err_t ret;

    for (;;) {
        ret = oxy_ensure_session();
        if (ret == ESP_OK) {
            oxy_close_file();
            ret = oxy_request(OXYII_OP_FILE_START, start, sizeof(start), 5000);
            if (ret == ESP_OK && s_resp_len < 4) ret = ESP_ERR_INVALID_RESPONSE;
        }
        if (ret == ESP_OK) {
            uint32_t sz = oxyii_le32(s_resp_buf);
            if (!sized) {
                ESP_LOGI(TAG, "File size: %lu bytes", (unsigned long)sz);
                if (sz == 0) { ret = ESP_ERR_INVALID_SIZE; break; }
                size = sz;
                sized = true;
                if (size_cb) size_cb(size, ctx);
            } else if (sz != size) {
                /* The file grew between connections: the night is still being
                 * recorded. The mule sees the short count against the size it
                 * was given and pulls it again later. */
                ESP_LOGW(TAG, "oxyii: %s changed size %lu -> %lu across a resume",
                         filename, (unsigned long)size, (unsigned long)sz);
                ret = ESP_ERR_INVALID_STATE;
                break;
            }
        }

        while (ret == ESP_OK && offset < size) {
            uint8_t o[4];
            oxyii_file_data_payload(offset, o);
            ret = oxy_request(OXYII_OP_FILE_DATA, o, sizeof(o), 5000);
            if (ret != ESP_OK) break;
            if (s_resp_len == 0) break;   /* the ring has nothing more */
            size_t n = s_resp_len;
            if (n > size - offset) n = size - offset;
            if (cb && !cb(s_resp_buf, n, offset, ctx)) {
                ESP_LOGE(TAG, "Stream callback error at %lu bytes", (unsigned long)offset);
                oxy_close_file();
                if (out_len) *out_len = offset;
                return ESP_FAIL;
            }
            offset += n;
        }
        if (ret == ESP_OK) break;

        /* Before the size is known the mule is still waiting on the file's
         * size, and a failed open is answered at once, as with the legacy
         * ring. */
        if (!sized || ++resumes > OXY_MAX_RESUMES) break;
        ESP_LOGW(TAG, "oxyii: %s stalled at %lu/%lu (%s); reconnect and resume %d/%d",
                 filename, (unsigned long)offset, (unsigned long)size,
                 esp_err_to_name(ret), resumes, OXY_MAX_RESUMES);
        /* A set bit left by the first link would end the wait below at once. */
        xEventGroupClearBits(s_cmd_events, EVT_CONNECTED);
        if (s_ready) {
            o2ring_ble_disconnect();
            for (int i = 0; i < 40 && s_ready; i++) vTaskDelay(pdMS_TO_TICKS(50));
        }
        ret = o2ring_ble_connect_and_wait(15000);
        if (ret == ESP_OK && !s_ready) ret = ESP_ERR_INVALID_STATE;
        if (ret != ESP_OK) break;
    }

    if (s_ready) oxy_close_file();
    if (out_len) *out_len = offset;
    if (ret == ESP_OK && offset < size) {
        ESP_LOGW(TAG, "oxyii: ring ended %s at %lu of %lu bytes",
                 filename, (unsigned long)offset, (unsigned long)size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (ret == ESP_OK) ESP_LOGI(TAG, "Streamed %lu bytes", (unsigned long)offset);
    return ret;
}

/* ── Public API ── */

esp_err_t o2ring_ble_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE (NimBLE)");

    if (!s_cmd_mutex)  s_cmd_mutex  = xSemaphoreCreateMutex();
    if (!s_cmd_events) s_cmd_events = xEventGroupCreate();
    if (!s_cmd_mutex || !s_cmd_events) return ESP_ERR_NO_MEM;

    memset(&s_info, 0, sizeof(s_info));
    memset(&s_peer_addr, 0, sizeof(s_peer_addr));
    s_have_cached = false;
    s_connecting_cached = false;
    s_connecting = false;
    s_ready = false;
    s_stopping = false;
    s_connected = false;
    s_scanning = false;
    s_service_found = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_write_handle = 0;
    s_notify_handle = 0;
    s_cccd_handle = 0;
    s_svc_start = s_svc_end = 0;
    s_reasm_len = 0;
    s_dl_active = false;
    s_dl_buf = NULL;
    s_dl_cb = NULL;
    s_dl_cb_ctx = NULL;
    s_dl_buf_size = 0;
    s_dl_offset = 0;
    s_dl_cb_error = false;
    /* A cached address is a legacy ring's; an O2Ring-S is matched by scan. */
    s_proto = RING_LEGACY;
    s_oxy_by_t8520 = false;
    s_oxy_skip_t8520 = false;
    s_oxy_session = false;
    s_oxy_wait_op = -1;

    o2ring_load_cached_addr();

    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Classic BT mem release failed: %s", esp_err_to_name(ret));
    }

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.sync_cb = o2ring_on_sync;
    ble_hs_cfg.reset_cb = o2ring_on_reset;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    /* Set BEFORE starting the host task, not after. The host task syncs almost
     * immediately and o2ring_on_sync() calls start_connection_attempt(), whose
     * first guard is `!s_stack_running` — the one exit from that function that
     * logs nothing. Set afterwards, sync always lost the race, the attempt
     * returned silently, and every request sat out its full connect timeout
     * having never scanned. */
    s_stack_running = true;
    nimble_port_freertos_init(o2ring_nimble_host_task);

    ESP_LOGI(TAG, "BLE ready (NimBLE host started)");
    return ESP_OK;
}

void o2ring_ble_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing BLE (NimBLE)");
    s_stopping = true;

    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }
    /* Close the link and WAIT for its disconnect event before stopping the host.
     * ble_hs_stop() terminates every open connection itself and counts the ones
     * it started in a static uint8 that every later disconnect event decrements
     * with no guard (ble_hs_stop.c). Terminating first and stopping at once made
     * its terminate fail with EALREADY (count untouched), our disconnect event
     * then wrapped the count to 255, and every deinit from then on sat out the
     * full host stop timeout ("247 connection(s) still up"), about 2 s per drop,
     * leaking a little heap each time. Handing the host a stack with no
     * connection keeps its count at zero: a drop then costs only the link's own
     * terminate latency. */
    if (s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        if (s_cmd_events) xEventGroupClearBits(s_cmd_events, EVT_DISCONNECTED);
        int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc == 0 && s_cmd_events) {
            EventBits_t b = xEventGroupWaitBits(s_cmd_events, EVT_DISCONNECTED,
                                                pdTRUE, pdTRUE, pdMS_TO_TICKS(1500));
            if (!(b & EVT_DISCONNECTED))
                ESP_LOGW(TAG, "no disconnect event within 1.5 s — stopping the host anyway");
        } else if (rc != 0) {
            ESP_LOGW(TAG, "terminate failed: %d — stopping the host anyway", rc);
        }
    }

    nimble_port_stop();
    nimble_port_deinit();

    s_stack_running = false;
    s_ready = false;
    s_connected = false;
    s_scanning = false;
    s_connecting = false;
    s_connecting_cached = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_write_handle = 0;
    s_notify_handle = 0;
    s_cccd_handle = 0;
    s_svc_start = s_svc_end = 0;

    ESP_LOGI(TAG, "BLE deinitialized — memory released");
}

esp_err_t o2ring_ble_start_scan(void)
{
    if (!s_stack_running) return ESP_ERR_INVALID_STATE;
    if (s_scanning || s_connected) return ESP_OK;
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER,
                        &(struct ble_gap_disc_params){
                            .itvl = 0x80,
                            .window = 0x40,
                            .filter_policy = 0,
                            .limited = 0,
                            .passive = 0,
                            .filter_duplicates = 1,
                            .disable_observer_mode = 0,
                        },
                        gap_event, NULL);
    if (rc == 0) s_scanning = true;
    return rc;
}

esp_err_t o2ring_ble_stop(void)
{
    s_stopping = true;
    s_connecting = false;
    if (s_scanning) ble_gap_disc_cancel();
    if (s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return ESP_OK;
}

bool o2ring_ble_is_connected(void)
{
    return s_ready;
}

esp_err_t o2ring_ble_refresh_info(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(15000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    if (s_proto == RING_OXYII) {
        esp_err_t oret = oxy_refresh_info();
        xSemaphoreGive(s_cmd_mutex);
        return oret;
    }

    s_info.valid = false;
    esp_err_t ret = send_and_wait(CMD_INFO, 0, NULL, 0, 10000);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_cmd_mutex);
        return ret;
    }

    size_t json_len = s_resp_len;
    while (json_len > 0 && s_resp_buf[json_len - 1] == 0x00) json_len--;
    if (json_len == 0) {
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char *json_str = malloc(json_len + 1);
    if (!json_str) {
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(json_str, s_resp_buf, json_len);
    json_str[json_len] = '\0';

    ESP_LOGI(TAG, "INFO response: %s", json_str);

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse INFO JSON");
        free(json_str);
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *bat = cJSON_GetObjectItem(root, "CurBAT");
    if (bat) {
        if (cJSON_IsNumber(bat)) s_info.battery = (uint8_t)bat->valueint;
        else if (cJSON_IsString(bat)) s_info.battery = (uint8_t)atoi(bat->valuestring);
    }

    cJSON *model = cJSON_GetObjectItem(root, "Model");
    if (model && cJSON_IsString(model))
        strncpy(s_info.model, model->valuestring, sizeof(s_info.model) - 1);

    cJSON *sn = cJSON_GetObjectItem(root, "SN");
    if (sn && cJSON_IsString(sn))
        strncpy(s_info.serial, sn->valuestring, sizeof(s_info.serial) - 1);

    cJSON *fl = cJSON_GetObjectItem(root, "FileList");
    s_info.file_count = 0;
    if (fl && cJSON_IsString(fl) && strlen(fl->valuestring) > 0) {
        char *list = strdup(fl->valuestring);
        if (list) {
            char *tok = strtok(list, ",");
            while (tok && s_info.file_count < O2RING_MAX_FILES) {
                while (*tok == ' ') tok++;
                if (strlen(tok) == 0) { tok = strtok(NULL, ","); continue; }
                strncpy(s_info.files[s_info.file_count].name, tok,
                        O2RING_MAX_FILENAME - 1);
                s_info.file_count++;
                tok = strtok(NULL, ",");
            }
            free(list);
        }
    }

    s_info.valid = true;
    cJSON_Delete(root);
    free(json_str);
    xSemaphoreGive(s_cmd_mutex);

    ESP_LOGI(TAG, "Device: %s SN=%s Batt=%u%% Files=%d",
             s_info.model, s_info.serial, s_info.battery, s_info.file_count);
    for (int i = 0; i < s_info.file_count; i++)
        ESP_LOGI(TAG, "  [%d] %s", i, s_info.files[i].name);

    return ESP_OK;
}

const o2ring_device_info_t *o2ring_ble_get_info(void)
{
    return &s_info;
}

void o2ring_ble_set_auto_reconnect(bool enable)
{
    s_auto_reconnect = enable;
}

esp_err_t o2ring_ble_disconnect(void)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return ESP_OK;
    return ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

esp_err_t o2ring_ble_connect_and_wait(uint32_t timeout_ms)
{
    if (s_ready) return ESP_OK;

    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        EventBits_t bits = xEventGroupWaitBits(s_cmd_events, EVT_CONNECTED,
                                               pdTRUE, pdTRUE,
                                               pdMS_TO_TICKS(500));
        if (bits & EVT_CONNECTED) return ESP_OK;
        if (s_ready) return ESP_OK;
        elapsed += 500;
    }

    if (s_scanning) ble_gap_disc_cancel();
    return ESP_ERR_TIMEOUT;
}

esp_err_t o2ring_ble_read_sensors(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    if (s_proto == RING_OXYII) {
        esp_err_t oret = oxy_read_sensors();
        xSemaphoreGive(s_cmd_mutex);
        return oret;
    }

    esp_err_t ret = send_and_wait(CMD_READ_SENSORS, 0, NULL, 0, 5000);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_cmd_mutex);
        return ret;
    }

    if (s_resp_len >= 13) {
        s_live.spo2 = s_resp_buf[0];
        s_live.hr = s_resp_buf[1];
        s_live.motion = s_resp_buf[9];
        s_live.vibration = s_resp_buf[12];
        s_live.timestamp = esp_timer_get_time();
        s_live.valid = (s_live.spo2 != 0xFF && s_live.hr != 0xFF);
        ESP_LOGI(TAG, "Live: SpO2=%u HR=%u motion=%u vib=%u",
                 s_live.spo2, s_live.hr, s_live.motion, s_live.vibration);
    }

    xSemaphoreGive(s_cmd_mutex);
    return ESP_OK;
}

const o2ring_live_t *o2ring_ble_get_live(void)
{
    return &s_live;
}

esp_err_t o2ring_ble_download_file(const char *filename,
                                   uint8_t *out_buf, size_t buf_size,
                                   size_t *out_len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    /* Nothing calls the buffered pull; the O2Ring-S is only streamed. */
    if (s_proto == RING_OXYII) return ESP_ERR_NOT_SUPPORTED;
    if (xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(30000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "Downloading %s", filename);

    size_t name_len = strlen(filename) + 1;
    esp_err_t ret = send_and_wait(CMD_FILE_OPEN, 0,
                                  (const uint8_t *)filename, name_len, 10000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FILE_OPEN failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_cmd_mutex);
        return ret;
    }

    if (s_resp_len < 4) {
        ESP_LOGE(TAG, "FILE_OPEN response too short (%u bytes)", (unsigned)s_resp_len);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t file_size = s_resp_buf[0] | ((uint32_t)s_resp_buf[1] << 8) |
                         ((uint32_t)s_resp_buf[2] << 16) | ((uint32_t)s_resp_buf[3] << 24);
    ESP_LOGI(TAG, "File size: %lu bytes", (unsigned long)file_size);

    if (file_size == 0 || file_size > buf_size) {
        ESP_LOGE(TAG, "File too large or empty: %lu (buf=%u)",
                 (unsigned long)file_size, (unsigned)buf_size);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    s_dl_buf = out_buf;
    s_dl_buf_size = buf_size;
    s_dl_offset = 0;
    s_dl_file_size = file_size;
    s_dl_block = 0;
    s_dl_active = true;
    s_dl_cb = NULL;
    s_dl_cb_ctx = NULL;
    s_dl_cb_error = false;

    xEventGroupClearBits(s_cmd_events, EVT_RESPONSE_READY);
    ret = send_cmd(CMD_FILE_READ, 0, NULL, 0);
    if (ret != ESP_OK) {
        s_dl_active = false;
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(s_cmd_mutex);
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(s_cmd_events, EVT_RESPONSE_READY,
                                           pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(120000));
    s_dl_active = false;

    if (!(bits & EVT_RESPONSE_READY)) {
        ESP_LOGE(TAG, "Download timeout at %lu/%lu bytes",
                 (unsigned long)s_dl_offset, (unsigned long)file_size);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_TIMEOUT;
    }

    *out_len = s_dl_offset;
    ESP_LOGI(TAG, "Downloaded %lu bytes", (unsigned long)s_dl_offset);

    xEventGroupClearBits(s_cmd_events, EVT_CLOSE_ACK);
    send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
    xEventGroupWaitBits(s_cmd_events, EVT_CLOSE_ACK,
                        pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));

    xSemaphoreGive(s_cmd_mutex);
    return ESP_OK;
}

esp_err_t o2ring_ble_download_file_stream(const char *filename,
                                          o2ring_size_cb_t size_cb,
                                          o2ring_chunk_cb_t cb, void *ctx,
                                          size_t *out_len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(30000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    if (s_proto == RING_OXYII) {
        esp_err_t oret = oxy_download_stream(filename, size_cb, cb, ctx, out_len);
        xSemaphoreGive(s_cmd_mutex);
        return oret;
    }

    ESP_LOGI(TAG, "Streaming download: %s", filename);

    size_t name_len = strlen(filename) + 1;
    esp_err_t ret = send_and_wait(CMD_FILE_OPEN, 0,
                                  (const uint8_t *)filename, name_len, 10000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FILE_OPEN failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_cmd_mutex);
        return ret;
    }

    if (s_resp_len < 4) {
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t file_size = s_resp_buf[0] | ((uint32_t)s_resp_buf[1] << 8) |
                         ((uint32_t)s_resp_buf[2] << 16) | ((uint32_t)s_resp_buf[3] << 24);
    ESP_LOGI(TAG, "File size: %lu bytes", (unsigned long)file_size);

    if (file_size == 0) {
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    /* The size is authoritative from here: hand it to the caller before any
     * chunk, so it can check the byte count at the end. */
    if (size_cb) size_cb(file_size, ctx);

    s_dl_buf = NULL;
    s_dl_buf_size = 0;
    s_dl_cb = cb;
    s_dl_cb_ctx = ctx;
    s_dl_cb_error = false;
    s_dl_offset = 0;
    s_dl_file_size = file_size;
    s_dl_block = 0;
    s_dl_active = true;

    xEventGroupClearBits(s_cmd_events, EVT_RESPONSE_READY);
    ret = send_cmd(CMD_FILE_READ, 0, NULL, 0);
    if (ret != ESP_OK) {
        s_dl_active = false;
        s_dl_cb = NULL;
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(s_cmd_mutex);
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(s_cmd_events, EVT_RESPONSE_READY,
                                           pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(120000));
    s_dl_active = false;
    s_dl_cb = NULL;

    if (s_dl_cb_error) {
        ESP_LOGE(TAG, "Stream callback error at %lu bytes", (unsigned long)s_dl_offset);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(s_cmd_mutex);
        return ESP_FAIL;
    }

    if (!(bits & EVT_RESPONSE_READY)) {
        ESP_LOGE(TAG, "Stream timeout at %lu/%lu bytes",
                 (unsigned long)s_dl_offset, (unsigned long)file_size);
        send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_TIMEOUT;
    }

    if (out_len) *out_len = s_dl_offset;
    ESP_LOGI(TAG, "Streamed %lu bytes", (unsigned long)s_dl_offset);

    xEventGroupClearBits(s_cmd_events, EVT_CLOSE_ACK);
    send_cmd(CMD_FILE_CLOSE, 0, NULL, 0);
    xEventGroupWaitBits(s_cmd_events, EVT_CLOSE_ACK,
                        pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));

    xSemaphoreGive(s_cmd_mutex);
    return ESP_OK;
}
