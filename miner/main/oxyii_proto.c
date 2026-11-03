/**
 * @file oxyii_proto.c
 * @brief O2Ring-S "OxyII" frames, payloads and replies (SDD-001). See oxyii_proto.h.
 */

#include "oxyii_proto.h"

#include <stdio.h>
#include <string.h>

/* MD5("lepucloud"), the protocol's fixed salt. A constant, so the chip carries
 * no MD5 just to compute it again. */
static const uint8_t LEPUCLOUD_MD5[16] = {
    0xc2, 0xa7, 0xcf, 0x50, 0xda, 0xfe, 0xd8, 0x85,
    0xa8, 0xf8, 0xf7, 0xea, 0xc4, 0x43, 0x35, 0xf3,
};

uint8_t oxyii_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

size_t oxyii_encode(uint8_t op, uint8_t seq, const uint8_t *payload, uint16_t len,
                    uint8_t *out, size_t cap)
{
    size_t total = (size_t)OXYII_OVERHEAD + len;
    if (total > cap) return 0;
    out[0] = OXYII_LEAD;
    out[1] = op;
    out[2] = (uint8_t)~op;
    out[3] = 0x00;
    out[4] = seq;
    out[5] = (uint8_t)(len & 0xFF);
    out[6] = (uint8_t)(len >> 8);
    if (len) memcpy(out + OXYII_HDR_LEN, payload, len);
    out[total - 1] = oxyii_crc8(out, total - 1);
    return total;
}

bool oxyii_find_frame(const uint8_t *buf, size_t len, size_t *skip, size_t *frame_len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != OXYII_LEAD) continue;
        size_t left = len - i;
        if (left < 3) { *skip = i; return false; }
        if (buf[i + 2] != (uint8_t)~buf[i + 1]) continue;
        if (left < OXYII_HDR_LEN) { *skip = i; return false; }
        uint16_t plen = oxyii_frame_len(buf + i);
        if (plen > OXYII_MAX_PAYLOAD) continue;
        size_t total = (size_t)OXYII_OVERHEAD + plen;
        if (left < total) { *skip = i; return false; }
        if (oxyii_crc8(buf + i, total - 1) != buf[i + total - 1]) continue;
        *skip = i;
        *frame_len = total;
        return true;
    }
    *skip = len;
    return false;
}

void oxyii_auth_payload(uint32_t ts, uint8_t out[OXYII_AUTH_LEN])
{
    uint8_t key[16];
    for (int i = 0; i < 8; i++) key[i] = LEPUCLOUD_MD5[i * 2];
    memcpy(key + 8, "0000", 4);
    for (int n = 0; n < 4; n++) key[12 + n] = (uint8_t)(ts >> n);
    for (int i = 0; i < 16; i++) out[i] = key[i] ^ LEPUCLOUD_MD5[i];
}

void oxyii_time_payload(const oxyii_datetime_t *t, uint8_t out[OXYII_TIME_LEN])
{
    out[0] = (uint8_t)(t->year & 0xFF);
    out[1] = (uint8_t)(t->year >> 8);
    out[2] = t->month;
    out[3] = t->day;
    out[4] = t->hour;
    out[5] = t->minute;
    out[6] = t->second;
    out[7] = 0x00;
}

uint32_t oxyii_datetime_epoch(const oxyii_datetime_t *t)
{
    /* Days from civil (Howard Hinnant), valid for every year the ring reports. */
    int y = (int)t->year - (t->month <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int mp = (t->month + 9) % 12;
    int doy = (153 * mp + 2) / 5 + t->day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400L + t->hour * 3600L + t->minute * 60L + t->second);
}

static bool datetime_plausible(const oxyii_datetime_t *t)
{
    return t->year >= 2020 && t->year <= 2099 && t->month >= 1 && t->month <= 12 &&
           t->day >= 1 && t->day <= 31 && t->hour <= 23 && t->minute <= 59 && t->second <= 59;
}

bool oxyii_parse_info(const uint8_t *p, size_t n, oxyii_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (n < 38) return false;

    for (int i = 0; i < 8; i++) {
        char c = (char)p[9 + i];
        if (c == '\0') break;
        out->firmware[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }

    out->clock.year   = (uint16_t)(p[24] | (p[25] << 8));
    out->clock.month  = p[26];
    out->clock.day    = p[27];
    out->clock.hour   = p[28];
    out->clock.minute = p[29];
    out->clock.second = p[30];
    out->clock_valid  = datetime_plausible(&out->clock);

    uint8_t sn_len = p[37];
    if (sn_len > 0 && (size_t)38 + sn_len <= n) {
        if (sn_len > sizeof(out->serial) - 1) sn_len = sizeof(out->serial) - 1;
        for (int i = 0; i < sn_len; i++) {
            char c = (char)p[38 + i];
            out->serial[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
    }
    return true;
}

bool oxyii_parse_auth_reply(const uint8_t *p, size_t n, uint8_t key[OXYII_KEY_LEN])
{
    if (n < 4 + OXYII_KEY_LEN) return false;
    uint8_t d[4 + OXYII_KEY_LEN];
    for (size_t i = 0; i < sizeof(d); i++) d[i] = p[i] ^ LEPUCLOUD_MD5[i % 16];
    if (d[0] != 0x01 || d[1] != OXYII_KEY_LEN) return false;
    memcpy(key, d + 4, OXYII_KEY_LEN);
    return true;
}

bool oxyii_reply_encrypted(uint8_t op)
{
    switch (op) {
    case 0x00: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
    case 0x11: case 0x12: case 0x13: case 0x14:
    case 0xE0: case 0xE1: case 0xE4:
    case 0xF1: case 0xF2: case 0xF3:
        return true;
    default:
        return false;
    }
}

size_t oxyii_encrypt(oxyii_block_fn enc, void *ctx, const uint8_t *p, size_t n,
                     uint8_t *out, size_t cap)
{
    size_t pad = 16 - (n % 16);
    size_t total = n + pad;
    if (total > cap) return 0;
    uint8_t block[16];
    for (size_t off = 0; off < total; off += 16) {
        for (size_t i = 0; i < 16; i++)
            block[i] = off + i < n ? p[off + i] : (uint8_t)pad;
        enc(ctx, block, out + off);
    }
    return total;
}

int oxyii_decrypt(oxyii_block_fn dec, void *ctx, uint8_t *p, size_t n)
{
    if (n == 0 || n % 16 != 0) return -1;
    uint8_t block[16];
    for (size_t off = 0; off < n; off += 16) {
        dec(ctx, p + off, block);
        memcpy(p + off, block, 16);
    }
    uint8_t pad = p[n - 1];
    if (pad < 1 || pad > 16) return -1;
    for (size_t i = 1; i <= pad; i++)
        if (p[n - i] != pad) return -1;
    return (int)(n - pad);
}

bool oxyii_parse_live(const uint8_t *p, size_t n, oxyii_live_t *out)
{
    memset(out, 0, sizeof(*out));
    if (n < 14) return false;
    uint16_t pr = (uint16_t)(p[8] | (p[9] << 8));
    out->run           = p[4];
    out->state         = p[5];
    out->spo2          = p[6];
    out->pi_x10        = p[7];
    out->hr            = pr < 0xFF ? (uint8_t)pr : 0xFF;
    out->motion        = p[11];
    out->battery_state = p[12];
    out->battery       = p[13];
    /* 0, 127 and 255 are the SDK's "no value" for SpO2; 0, 255, 511 and
     * 65535 for the pulse rate. Only sensor state 1 is a finger on the ring. */
    out->reading = out->state == 1 &&
                   out->spo2 >= 1 && out->spo2 <= 100 &&
                   pr > 0 && pr < 0xFF;
    return true;
}

static bool is_ring_name(const uint8_t *s)
{
    for (int i = 0; i < OXYII_NAME_LEN; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

int oxyii_parse_file_list(const uint8_t *p, size_t n, char *names, size_t name_cap,
                          int max, int *total)
{
    if (total) *total = 0;
    if (n < 1 || max <= 0 || name_cap < OXYII_NAME_LEN + sizeof(OXYII_FILE_EXT)) return 0;
    int count = p[0];
    if (total) *total = count;

    int slots = (int)((n - 1) / OXYII_NAME_SLOT);
    if (slots > count) slots = count;

    /* Newest last on the ring: keep the last max valid slots. */
    int valid = 0;
    for (int i = 0; i < slots; i++)
        if (is_ring_name(p + 1 + i * OXYII_NAME_SLOT)) valid++;
    int drop = valid > max ? valid - max : 0;

    int out = 0;
    for (int i = 0; i < slots; i++) {
        const uint8_t *s = p + 1 + i * OXYII_NAME_SLOT;
        if (!is_ring_name(s)) continue;
        if (drop > 0) { drop--; continue; }
        char *dst = names + (size_t)out * name_cap;
        memcpy(dst, s, OXYII_NAME_LEN);
        memcpy(dst + OXYII_NAME_LEN, OXYII_FILE_EXT, sizeof(OXYII_FILE_EXT));
        out++;
    }
    return out;
}

bool oxyii_file_start_payload(const char *filename, uint8_t out[OXYII_FILE_START_LEN])
{
    size_t len = strlen(filename);
    size_t ext = sizeof(OXYII_FILE_EXT) - 1;
    if (len == OXYII_NAME_LEN + ext && strcmp(filename + OXYII_NAME_LEN, OXYII_FILE_EXT) == 0)
        len = OXYII_NAME_LEN;
    if (len != OXYII_NAME_LEN || !is_ring_name((const uint8_t *)filename)) return false;
    memset(out, 0, OXYII_FILE_START_LEN);
    memcpy(out, filename, OXYII_NAME_LEN);
    /* [16..19] u32 file type: 0 = oximetry, the only kind the ring stores. */
    return true;
}

void oxyii_file_data_payload(uint32_t offset, uint8_t out[4])
{
    out[0] = (uint8_t)offset;
    out[1] = (uint8_t)(offset >> 8);
    out[2] = (uint8_t)(offset >> 16);
    out[3] = (uint8_t)(offset >> 24);
}

bool oxyii_name_matches(const char *lower)
{
    return strncmp(lower, "s8-aw", 5) == 0 || strncmp(lower, "t8520", 5) == 0;
}
