#include "as11_fig.h"

#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

/* ── CRC-32, the zlib one (poly 0xEDB88320 reflected, init and xor 0xFFFFFFFF) ── */

uint32_t as11_crc32(const uint8_t *data, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

static void put_le16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint16_t get_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint8_t AS11_MAGIC[4] = {0xBE, 0xBA, 0xFE, 0xCA};

size_t as11_fig_build(uint16_t type, const uint8_t *payload, size_t len,
                      uint8_t *out, size_t cap)
{
    if (len > 0xFFFF || cap < AS11_FIG_HDR_LEN + len) return 0;
    memcpy(out, AS11_MAGIC, 4);
    put_le16(out + 4, type);
    put_le16(out + 6, (uint16_t)len);
    put_le32(out + 8, as11_crc32(payload, len));
    put_le32(out + 12, as11_crc32(out + 4, 8));
    if (len) memcpy(out + AS11_FIG_HDR_LEN, payload, len);
    return AS11_FIG_HDR_LEN + len;
}

/* ── reassembly ── */

void as11_reasm_reset(as11_reasm_t *r) { r->len = 0; r->consumed = 0; }

bool as11_reasm_feed(as11_reasm_t *r, const uint8_t *data, size_t len)
{
    if (len > sizeof(r->buf)) return false;
    if (r->len + len > sizeof(r->buf)) {
        /* Older than a whole buffer with no frame in it: it is noise. Keep the
         * tail, which may hold the start of the frame being received. */
        size_t keep = sizeof(r->buf) - len;
        memmove(r->buf, r->buf + (r->len - keep), keep);
        r->len = keep;
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    return true;
}

/* Offset of the first magic at or after `from` whose header checks, or -1. */
static long next_header(const as11_reasm_t *r, size_t from)
{
    for (size_t i = from; i + AS11_FIG_HDR_LEN <= r->len; i++) {
        if (memcmp(r->buf + i, AS11_MAGIC, 4) != 0) continue;
        if (as11_crc32(r->buf + i + 4, 8) == get_le32(r->buf + i + 12)) return (long)i;
    }
    return -1;
}

static void drop(as11_reasm_t *r, size_t n)
{
    if (n >= r->len) { r->len = 0; return; }
    memmove(r->buf, r->buf + n, r->len - n);
    r->len -= n;
}

bool as11_reasm_next(as11_reasm_t *r, uint16_t *type, const uint8_t **payload, size_t *plen)
{
    if (r->consumed) {
        drop(r, r->consumed);
        r->consumed = 0;
    }
    for (;;) {
        /* Find a magic; what precedes it can never start a frame. */
        size_t i = 0;
        while (i + 4 <= r->len && memcmp(r->buf + i, AS11_MAGIC, 4) != 0) i++;
        if (i + 4 > r->len) {
            /* No magic: keep at most the last three bytes (a magic split). */
            if (r->len > 3) drop(r, r->len - 3);
            return false;
        }
        if (i) drop(r, i);
        if (r->len < AS11_FIG_HDR_LEN) return false;

        if (as11_crc32(r->buf + 4, 8) != get_le32(r->buf + 12)) {
            drop(r, 1);           /* a false magic: resync */
            continue;
        }
        size_t ln = get_le16(r->buf + 6);
        if (ln > AS11_FIG_MAX_PAYLOAD) { drop(r, 1); continue; }
        if (r->len < AS11_FIG_HDR_LEN + ln) {
            /* Still waiting. If a valid header already sits inside what we
             * have, a notification of this frame was lost: skip to it. */
            long nxt = next_header(r, 1);
            if (nxt < 0) return false;
            drop(r, (size_t)nxt);
            continue;
        }
        if (as11_crc32(r->buf + AS11_FIG_HDR_LEN, ln) != get_le32(r->buf + 8)) {
            /* The declared length ran into the next frame (a lost piece):
             * skip only this header, so the next frame is not eaten. */
            drop(r, 1);
            continue;
        }
        *type    = get_le16(r->buf + 4);
        *payload = r->buf + AS11_FIG_HDR_LEN;
        *plen    = ln;
        /* The caller reads the payload in place; it is dropped on the next call. */
        r->consumed = AS11_FIG_HDR_LEN + ln;
        return true;
    }
}

/* ── session cipher ── */

void as11_cipher_init(as11_cipher_t *c, const uint8_t master_key[32],
                      const uint8_t *nonce, size_t nonce_len)
{
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, master_key, 32);
    mbedtls_sha256_update(&sha, nonce, nonce_len);
    mbedtls_sha256_finish(&sha, c->key);
    mbedtls_sha256_free(&sha);
}

size_t as11_cipher_encrypt(const as11_cipher_t *c, const uint8_t iv[16],
                           const uint8_t *json, size_t len, uint8_t *out, size_t cap)
{
    if (len > 0xFFFF) return 0;
    size_t inner = 2 + len;
    size_t padded = (inner + 15) & ~(size_t)15;
    if (cap < 16 + padded) return 0;
    uint8_t *plain = out + 16;   /* encrypt in place after the IV */
    memset(plain, 0, padded);
    put_le16(plain, (uint16_t)len);
    memcpy(plain + 2, json, len);
    memcpy(out, iv, 16);

    uint8_t ivc[16];
    memcpy(ivc, iv, 16);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, c->key, 256);
    int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, ivc, plain, plain);
    mbedtls_aes_free(&aes);
    return rc == 0 ? 16 + padded : 0;
}

size_t as11_cipher_decrypt(const as11_cipher_t *c, const uint8_t *payload, size_t plen,
                           uint8_t *out, size_t cap)
{
    if (plen < 32 || (plen - 16) % 16 != 0) return 0;
    size_t ct = plen - 16;
    if (cap < ct + 1) return 0;
    uint8_t ivc[16];
    memcpy(ivc, payload, 16);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, c->key, 256);
    int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ct, ivc, payload + 16, out);
    mbedtls_aes_free(&aes);
    if (rc != 0) return 0;
    size_t ln = get_le16(out);
    if (ln + 2 > ct) return 0;
    memmove(out, out + 2, ln);
    out[ln] = '\0';
    return ln;
}

bool as11_challenge_response(const uint8_t master_key[32], const char *challenge_hex,
                             char out[65])
{
    uint8_t ch[64];
    size_t n;
    if (!as11_hex_decode(challenge_hex, ch, sizeof(ch), &n)) return false;
    uint8_t mac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, master_key, 32, ch, n, mac) != 0) return false;
    as11_hex_encode(mac, 32, out);
    return true;
}

/* ── helpers ── */

void as11_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xF];
    }
    out[2 * n] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool as11_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *n)
{
    size_t len = strlen(hex);
    if (len % 2 || len / 2 > cap) return false;
    for (size_t i = 0; i < len; i += 2) {
        int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *n = len / 2;
    return true;
}

size_t as11_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        else return 0;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return 0;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return n;
}
