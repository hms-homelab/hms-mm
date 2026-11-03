/**
 * @file as11_fig.h
 * @brief The AirSense 11's "FIG" Bluetooth framing and session crypto (SDD-001).
 *
 * Pure functions over bytes, no BLE, no ESP-IDF beyond mbedTLS, so the host
 * test exercises every byte the miner puts on the air. The GATT side lives in
 * as11_ble.c; the JSON-RPC bodies are built there with cJSON.
 *
 * Every message, both directions, is one frame (PROTOCOL.md §2 of
 * github.com/maroliar/hass-resmed-airsense, MIT):
 *
 *   0   4  magic   CA FE BA BE, on the wire BE BA FE CA
 *   4   2  type    u16 LE: 0x0393 app->machine clear, 0x0392 machine->app clear,
 *                  0x0397 app->machine encrypted, 0x0396 machine->app encrypted;
 *                  bit 0x0004 says encrypted
 *   6   2  length  u16 LE, of the payload
 *   8   4  pcrc    CRC-32 (zlib) of the payload, LE
 *   12  4  hcrc    CRC-32 of bytes 4..11, LE
 *   16  n  payload JSON, or IV(16) || AES-256-CBC(len(2 LE) || JSON || zero pad)
 *
 * A notification may carry a piece of a frame; the reassembler joins them and
 * drops a frame whose CRC fails, resyncing on the next magic.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AS11_FIG_HDR_LEN     16
#define AS11_T_DEV_PLAIN     0x0392
#define AS11_T_APP_PLAIN     0x0393
#define AS11_T_DEV_ENC       0x0396
#define AS11_T_APP_ENC       0x0397
#define AS11_T_ENCRYPTED_BIT 0x0004

/* The largest frame the machine sends: a spool fragment of maxFragmentSize
 * 2808 bytes is base64 (3744) inside JSON inside the cipher. */
#define AS11_FIG_MAX_PAYLOAD 4096

uint32_t as11_crc32(const uint8_t *data, size_t len);

/* Builds a frame into out. Returns its length, or 0 if it does not fit. */
size_t as11_fig_build(uint16_t type, const uint8_t *payload, size_t len,
                      uint8_t *out, size_t cap);

/* Joins notifications into frames. */
typedef struct {
    uint8_t  buf[AS11_FIG_HDR_LEN + AS11_FIG_MAX_PAYLOAD];
    size_t   len;
    size_t   consumed;   /* the frame handed out last time, dropped on the next call */
} as11_reasm_t;

void as11_reasm_reset(as11_reasm_t *r);

/* Feeds bytes; returns true with *type, *payload, *plen set when one whole,
 * valid frame is ready (the payload points into the reassembler and is valid
 * until the next feed/next call). Call as11_reasm_next() again until it returns
 * false: one notification may complete more than one frame. */
bool as11_reasm_feed(as11_reasm_t *r, const uint8_t *data, size_t len);
bool as11_reasm_next(as11_reasm_t *r, uint16_t *type, const uint8_t **payload, size_t *plen);

/* ── the session cipher ──
 * sessionKey = SHA-256(masterPairKey || nonce); payload = IV || AES-256-CBC(
 * len(2 LE) || JSON || zero padding to 16). */
typedef struct {
    uint8_t key[32];
} as11_cipher_t;

void as11_cipher_init(as11_cipher_t *c, const uint8_t master_key[32],
                      const uint8_t *nonce, size_t nonce_len);

/* Encrypts json (len bytes) with the given 16-byte IV into out; returns the
 * payload length (16 + padded), or 0 if out is too small. The caller supplies
 * the IV so the host test is deterministic; the miner draws it from esp_random. */
size_t as11_cipher_encrypt(const as11_cipher_t *c, const uint8_t iv[16],
                           const uint8_t *json, size_t len, uint8_t *out, size_t cap);

/* Decrypts a payload into out (the JSON, NUL-terminated); returns its length
 * or 0 when the payload is malformed or does not fit. */
size_t as11_cipher_decrypt(const as11_cipher_t *c, const uint8_t *payload, size_t plen,
                           uint8_t *out, size_t cap);

/* HMAC-SHA256(masterPairKey, challenge) as upper-case hex (64 chars + NUL):
 * the CheckSessionIntegrity response. challenge_hex is the machine's hex. */
bool as11_challenge_response(const uint8_t master_key[32], const char *challenge_hex,
                             char out[65]);

/* ── small helpers shared with the SRP and the BLE side ── */
void   as11_hex_encode(const uint8_t *in, size_t n, char *out);       /* upper-case, NUL */
bool   as11_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *n);
size_t as11_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t cap);
