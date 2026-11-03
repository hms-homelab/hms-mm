/**
 * @file oxyii_proto.h
 * @brief The O2Ring-S (T8520) "OxyII" protocol: frames, payloads, replies (SDD-001).
 *
 * Pure functions, no BLE and no ESP-IDF, so the host test exercises every byte
 * the miner puts on the air. The BLE side lives in o2ring_ble_nimble.c.
 *
 * Frame, both directions:
 *   A5 | cmd | ~cmd | flag | seq | len_lo | len_hi | payload | crc
 * flag is 0 on a request and 1 on a reply; crc is CRC-8 poly 0x07, init 0,
 * over every byte before it. The legacy O2Ring's 0xAA frames are a different
 * codec and stay in o2ring_ble_nimble.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OXYII_LEAD          0xA5
#define OXYII_HDR_LEN       7
#define OXYII_OVERHEAD      (OXYII_HDR_LEN + 1)
/* A file chunk is at most 512 bytes; nothing the ring sends is larger, so a
 * header claiming more is noise, not a frame to wait for. */
#define OXYII_MAX_PAYLOAD   1024

#define OXYII_OP_GET_CONFIG 0x00
#define OXYII_OP_LIVE       0x04   /* 24-byte header with SpO2, HR, motion, battery */
#define OXYII_OP_SETUP      0x10   /* payload 00, required after auth */
#define OXYII_OP_SET_TIME   0xC0
#define OXYII_OP_GET_INFO   0xE1
#define OXYII_OP_GET_BATTERY 0xE4  /* reply byte [1] is the percentage */
#define OXYII_OP_FILE_LIST  0xF1
#define OXYII_OP_FILE_START 0xF2
#define OXYII_OP_FILE_DATA  0xF3
#define OXYII_OP_FILE_END   0xF4
#define OXYII_OP_AUTH       0xFF   /* one-way, never answered */

#define OXYII_AUTH_LEN       16
#define OXYII_TIME_LEN       8
#define OXYII_FILE_START_LEN 20
#define OXYII_NAME_LEN       14    /* YYYYMMDDhhmmss */
#define OXYII_NAME_SLOT      16
/* The ring's names carry no extension. The miner adds this one so every layer
 * downstream (the mule, hms-cpap) tells the format from the name. */
#define OXYII_FILE_EXT       ".o2s"

uint8_t oxyii_crc8(const uint8_t *data, size_t len);

/* Builds a request frame into out. Returns its length, or 0 if it does not fit. */
size_t oxyii_encode(uint8_t op, uint8_t seq, const uint8_t *payload, uint16_t len,
                    uint8_t *out, size_t cap);

/* Looks for one whole, valid frame in buf. *skip is always set to the bytes in
 * front that can never start a frame and may be dropped. Returns true with
 * *frame_len set when a frame starts at buf + *skip; false means wait for more. */
bool oxyii_find_frame(const uint8_t *buf, size_t len, size_t *skip, size_t *frame_len);

static inline uint8_t  oxyii_frame_op(const uint8_t *f)  { return f[1]; }
static inline uint16_t oxyii_frame_len(const uint8_t *f) { return (uint16_t)(f[5] | (f[6] << 8)); }
static inline const uint8_t *oxyii_frame_payload(const uint8_t *f) { return f + OXYII_HDR_LEN; }

/* cmd 0xFF: the session key XOR MD5("lepucloud"), with the portable serial
 * prefix "0000" and ts in its bytes 12..15 as (ts >> 0..3), as the ring expects. */
void oxyii_auth_payload(uint32_t ts, uint8_t out[OXYII_AUTH_LEN]);

/* The encrypted session. Older rings never answer 0xFF and stay in plaintext.
 * Newer ones (firmware 1.13.x, branch 2D010001) answer it with a key blob,
 * and from then on payloads are AES-128-ECB with PKCS7 padding in both
 * directions: a request's when it is not empty (0xFF never), a reply's for the
 * opcodes oxyii_reply_encrypted() names. Only the payload is ciphertext; the
 * envelope and its CRC stay valid, so a client that misses the switch reads
 * ciphertext as vitals and file names. Layout from the vendor SDK
 * (OxyIIBleInterface) as documented by khromov/wellue-o2ring-web-ui and
 * Plantucha/Tepna, which agree. */
#define OXYII_KEY_LEN 16

/* cmd 0xFF reply: >= 20 bytes that XOR MD5("lepucloud") to [1, 16, ?, ?,
 * key16] carry the session key. Anything else, a 16-byte reply included
 * (some rings send one and then work in plaintext), is no key: false. */
bool oxyii_parse_auth_reply(const uint8_t *p, size_t n, uint8_t key[OXYII_KEY_LEN]);

/* Does this opcode's reply payload arrive encrypted in an encrypted session? */
bool oxyii_reply_encrypted(uint8_t op);

/* One AES-128 block, in place of a crypto library here: the miner passes
 * mbedtls, the host test another implementation. */
typedef void (*oxyii_block_fn)(void *ctx, const uint8_t in[16], uint8_t out[16]);

/* PKCS7-pads n bytes and encrypts them block by block into out. Returns the
 * ciphertext length (n rounded up to the next 16, a whole block when n is a
 * multiple of 16), or 0 if it does not fit in cap. */
size_t oxyii_encrypt(oxyii_block_fn enc, void *ctx, const uint8_t *p, size_t n,
                     uint8_t *out, size_t cap);

/* Decrypts n bytes in place and strips the PKCS7 padding. Returns the
 * plaintext length, or -1 when n is not a whole number of blocks or the
 * padding is not valid (the wrong key, or a reply that was not encrypted). */
int oxyii_decrypt(oxyii_block_fn dec, void *ctx, uint8_t *p, size_t n);

typedef struct {
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
} oxyii_datetime_t;

/* cmd 0xC0: year LE, month, day, hour, minute, second, 0. */
void oxyii_time_payload(const oxyii_datetime_t *t, uint8_t out[OXYII_TIME_LEN]);

/* Seconds since 1970 for t taken as UTC, without the C library's time zone. */
uint32_t oxyii_datetime_epoch(const oxyii_datetime_t *t);

typedef struct {
    char             firmware[9];   /* "2D010002" */
    char             serial[24];
    oxyii_datetime_t clock;         /* the ring's own RTC, as it was set */
    bool             clock_valid;
} oxyii_info_t;

/* cmd 0xE1 reply (60 bytes on the firmware documented). */
bool oxyii_parse_info(const uint8_t *p, size_t n, oxyii_info_t *out);

typedef struct {
    uint8_t  run;       /* 0 prep, 1 measure prep, 2 measuring, 3 ended */
    uint8_t  state;     /* sensor: 0 no finger, 1 normal, 2 probe off, 3 fault */
    uint8_t  spo2, hr, motion, battery, battery_state;
    uint8_t  pi_x10;    /* perfusion index, tenths of a percent */
    bool     reading;   /* a finger and a valid SpO2 and pulse rate */
} oxyii_live_t;

/* cmd 0x04 reply, the vendor SDK's RtParam: [4] run status, [5] sensor state,
 * [6] SpO2, [7] PI x10, [8..9] pulse rate u16 LE, [11] motion, [12] battery
 * state, [13] battery %. [7] is perfusion, not motion, and [9] is the pulse
 * rate's high byte, not padding: the first write-up had both wrong. */
bool oxyii_parse_live(const uint8_t *p, size_t n, oxyii_live_t *out);

/* cmd 0xF1 reply: u8 count, then 16-byte slots of a 14-digit name. Writes the
 * NEWEST max names (the list runs oldest first) as "<name>.o2s" into names,
 * each name_cap bytes, oldest of them first. *total gets the ring's count.
 * Slots that are not 14 digits are skipped. Returns the number written. */
int oxyii_parse_file_list(const uint8_t *p, size_t n, char *names, size_t name_cap,
                          int max, int *total);

/* cmd 0xF2 payload from "<name>" or "<name>.o2s"; false if not a ring name. */
bool oxyii_file_start_payload(const char *filename, uint8_t out[OXYII_FILE_START_LEN]);

/* cmd 0xF3 payload: the u32 LE offset. */
void oxyii_file_data_payload(uint32_t offset, uint8_t out[4]);

static inline uint32_t oxyii_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A lowercased advertised name that is an O2Ring-S: "s8-aw ..." while idle or
 * syncing, "t8520_xxxx" while worn. */
bool oxyii_name_matches(const char *lower);
