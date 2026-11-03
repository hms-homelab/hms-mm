/*
 * SDD-001: host test for the O2Ring-S (OxyII) codec.
 *
 *   cc -I miner/main -I$(brew --prefix openssl@3)/include \
 *      miner/test/host/test_oxyii_proto.c miner/main/oxyii_proto.c \
 *      -L$(brew --prefix openssl@3)/lib -lcrypto -o /tmp/t && /tmp/t
 *
 * The CRC fixture is the protocol write-up's own; the auth and frame vectors
 * were produced by its reference implementation (oxyii_protocol.py). The AES
 * vectors come from `openssl enc -aes-128-ecb` (PKCS7 by default), so the
 * padding is checked against an implementation other than ours. OpenSSL only
 * stands in for the AES block the miner gets from mbedtls.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>

#include "oxyii_proto.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static void hex(const char *s, uint8_t *out, size_t *n)
{
    *n = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[(*n)++] = (uint8_t)v;
    }
}

static void test_crc_fixture(void)
{
    const uint8_t get_info[] = {0xA5, 0xE1, 0x1E, 0x00, 0x02, 0x00, 0x00};
    CHECK(oxyii_crc8(get_info, sizeof(get_info)) == 0xBF);
}

static void test_encode_get_info(void)
{
    uint8_t f[16];
    size_t n = oxyii_encode(OXYII_OP_GET_INFO, 2, NULL, 0, f, sizeof(f));
    const uint8_t want[] = {0xA5, 0xE1, 0x1E, 0x00, 0x02, 0x00, 0x00, 0xBF};
    CHECK(n == sizeof(want));
    CHECK(memcmp(f, want, sizeof(want)) == 0);
    CHECK(oxyii_encode(OXYII_OP_GET_INFO, 2, NULL, 0, f, 7) == 0);   /* no room */
}

static void test_auth_frame(void)
{
    uint8_t p[OXYII_AUTH_LEN];
    oxyii_auth_payload(1791158400u, p);
    uint8_t f[32];
    size_t n = oxyii_encode(OXYII_OP_AUTH, 0, p, sizeof(p), f, sizeof(f));
    uint8_t want[32];
    size_t wn;
    hex("a5ff00000010000068158872091cb098c8c7da440315e3a2", want, &wn);
    CHECK(n == wn);
    CHECK(memcmp(f, want, wn) == 0);
}

static void test_find_frame(void)
{
    uint8_t reply[32];
    size_t rn;
    hex("a5f30c01070a0000010203040506070809b5", reply, &rn);   /* F3 reply, seq 7, 10 bytes */

    size_t skip, len;
    CHECK(oxyii_find_frame(reply, rn, &skip, &len));
    CHECK(skip == 0 && len == rn);
    CHECK(oxyii_frame_op(reply) == OXYII_OP_FILE_DATA);
    CHECK(oxyii_frame_len(reply) == 10);
    CHECK(oxyii_frame_payload(reply)[9] == 0x09);

    /* Garbage in front, including a stray lead byte. */
    uint8_t buf[64];
    const uint8_t junk[] = {0x00, 0xA5, 0x11, 0x42};
    memcpy(buf, junk, sizeof(junk));
    memcpy(buf + sizeof(junk), reply, rn);
    CHECK(oxyii_find_frame(buf, sizeof(junk) + rn, &skip, &len));
    CHECK(skip == sizeof(junk) && len == rn);

    /* Split across two notifications: wait, and drop nothing that matters. */
    CHECK(!oxyii_find_frame(reply, 9, &skip, &len));
    CHECK(skip == 0);
    CHECK(!oxyii_find_frame(reply, 2, &skip, &len));
    CHECK(skip == 0);

    /* A bad CRC is not a frame. */
    uint8_t bad[32];
    memcpy(bad, reply, rn);
    bad[rn - 1] ^= 0xFF;
    CHECK(!oxyii_find_frame(bad, rn, &skip, &len));
    CHECK(skip == rn);

    /* A header claiming a huge payload is noise, not something to wait for. */
    const uint8_t huge[] = {0xA5, 0xF3, 0x0C, 0x01, 0x00, 0xFF, 0xFF};
    CHECK(!oxyii_find_frame(huge, sizeof(huge), &skip, &len));
    CHECK(skip == sizeof(huge));
}

static void test_time(void)
{
    oxyii_datetime_t t = {2026, 10, 5, 23, 4, 59};
    uint8_t p[OXYII_TIME_LEN];
    oxyii_time_payload(&t, p);
    const uint8_t want[] = {0xEA, 0x07, 10, 5, 23, 4, 59, 0x00};
    CHECK(memcmp(p, want, sizeof(want)) == 0);

    oxyii_datetime_t midnight = {2026, 10, 5, 0, 0, 0};
    CHECK(oxyii_datetime_epoch(&midnight) == 1791158400u);
    oxyii_datetime_t leap = {2028, 2, 29, 12, 0, 0};
    CHECK(oxyii_datetime_epoch(&leap) == 1835438400u);
}

static void test_info(void)
{
    uint8_t p[60] = {0};
    p[0] = 0x42;
    memcpy(p + 9, "2D010002", 8);
    p[17] = 0x01;
    p[24] = 0xEA; p[25] = 0x07; p[26] = 10; p[27] = 5; p[28] = 22; p[29] = 30; p[30] = 1;
    p[37] = 10;
    memcpy(p + 38, "00X0000001", 10);

    oxyii_info_t info;
    CHECK(oxyii_parse_info(p, sizeof(p), &info));
    CHECK(strcmp(info.firmware, "2D010002") == 0);
    CHECK(strcmp(info.serial, "00X0000001") == 0);
    CHECK(info.clock_valid);
    CHECK(info.clock.year == 2026 && info.clock.month == 10 && info.clock.day == 5);
    CHECK(info.clock.hour == 22 && info.clock.minute == 30 && info.clock.second == 1);

    /* A ring whose clock was never set reports nothing usable. */
    p[24] = 0; p[25] = 0;
    CHECK(oxyii_parse_info(p, sizeof(p), &info));
    CHECK(!info.clock_valid);

    CHECK(!oxyii_parse_info(p, 30, &info));
}

static void test_live(void)
{
    /* The vendor SDK's RtParam: [4] run, [5] sensor, [6] SpO2, [7] PI x10,
     * [8..9] PR u16 LE, [11] motion, [12] battery state, [13] battery %. */
    uint8_t p[40] = {0};
    p[4] = 2; p[5] = 1; p[6] = 96; p[7] = 13; p[8] = 62; p[11] = 4; p[12] = 0; p[13] = 80;
    oxyii_live_t l;
    CHECK(oxyii_parse_live(p, sizeof(p), &l));
    CHECK(l.reading && l.spo2 == 96 && l.hr == 62 && l.pi_x10 == 13 && l.motion == 4 &&
          l.battery == 80 && l.run == 2 && l.state == 1);

    p[5] = 0;                       /* no finger */
    CHECK(oxyii_parse_live(p, sizeof(p), &l) && !l.reading);
    p[5] = 2;                       /* probe off is not a finger either */
    CHECK(oxyii_parse_live(p, sizeof(p), &l) && !l.reading);
    p[5] = 1; p[8] = 0xFF;          /* PR "no value" */
    CHECK(oxyii_parse_live(p, sizeof(p), &l) && !l.reading);
    p[8] = 0xFF; p[9] = 0x01;       /* 511, the other "no value"; [9] is the high byte */
    CHECK(oxyii_parse_live(p, sizeof(p), &l) && !l.reading && l.hr == 0xFF);
    p[8] = 62; p[9] = 0; p[6] = 127; /* SpO2 "no value" */
    CHECK(oxyii_parse_live(p, sizeof(p), &l) && !l.reading);
    p[6] = 96;
    CHECK(oxyii_parse_live(p, sizeof(p), &l) && l.reading);

    CHECK(oxyii_parse_live(p, 14, &l));
    CHECK(!oxyii_parse_live(p, 13, &l));
}

/* AES-128 ECB one block, no padding: the stand-in for mbedtls. */
static void ossl_block(void *ctx, const uint8_t in[16], uint8_t out[16], int enc)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0;
    EVP_CipherInit_ex(c, EVP_aes_128_ecb(), NULL, (const uint8_t *)ctx, NULL, enc);
    EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_CipherUpdate(c, out, &n, in, 16);
    EVP_CIPHER_CTX_free(c);
}
static void ossl_enc(void *ctx, const uint8_t in[16], uint8_t out[16]) { ossl_block(ctx, in, out, 1); }
static void ossl_dec(void *ctx, const uint8_t in[16], uint8_t out[16]) { ossl_block(ctx, in, out, 0); }

static void test_aes(void)
{
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
    uint8_t want[64], out[64];
    size_t wn;

    /* A 20-byte READ_FILE_START payload: two blocks, 12 bytes of 0x0c pad. */
    uint8_t start[OXYII_FILE_START_LEN];
    CHECK(oxyii_file_start_payload("20261006013000.o2s", start));
    hex("1b10db80b5762ce93d102cd76b484e0ed2591ea1e1a2a00b753e691052e40191", want, &wn);
    CHECK(oxyii_encrypt(ossl_enc, key, start, sizeof(start), out, sizeof(out)) == wn);
    CHECK(memcmp(out, want, wn) == 0);
    CHECK(oxyii_decrypt(ossl_dec, key, out, wn) == OXYII_FILE_START_LEN);
    CHECK(memcmp(out, start, sizeof(start)) == 0);

    /* One byte (setup's 00): one block. */
    const uint8_t zero = 0;
    hex("4c4d10e1f5542fef3e2da31ff4b4471a", want, &wn);
    CHECK(oxyii_encrypt(ossl_enc, key, &zero, 1, out, sizeof(out)) == 16);
    CHECK(memcmp(out, want, 16) == 0);

    /* A whole block takes a whole block of padding. */
    hex("281567ab2f4cf0d73d3198225b8b8393954f64f2e4e86e9eee82d20216684899", want, &wn);
    CHECK(oxyii_encrypt(ossl_enc, key, (const uint8_t *)"0123456789abcdef", 16,
                        out, sizeof(out)) == 32);
    CHECK(memcmp(out, want, 32) == 0);
    CHECK(oxyii_decrypt(ossl_dec, key, out, 32) == 16);
    CHECK(memcmp(out, "0123456789abcdef", 16) == 0);

    /* Too small an output, a partial block, and a wrong key all refuse. */
    CHECK(oxyii_encrypt(ossl_enc, key, start, sizeof(start), out, 31) == 0);
    hex("1b10db80b5762ce93d102cd76b484e0ed2591ea1e1a2a00b753e691052e40191", want, &wn);
    CHECK(oxyii_decrypt(ossl_dec, key, want, 20) == -1);
    uint8_t other[16] = {0};
    CHECK(oxyii_decrypt(ossl_dec, other, want, wn) == -1);
}

static void test_auth_reply(void)
{
    static const uint8_t lepu[16] = {
        0xc2, 0xa7, 0xcf, 0x50, 0xda, 0xfe, 0xd8, 0x85,
        0xa8, 0xf8, 0xf7, 0xea, 0xc4, 0x43, 0x35, 0xf3,
    };
    uint8_t plain[20] = {0x01, 0x10, 0x00, 0x00};
    for (int i = 0; i < 16; i++) plain[4 + i] = (uint8_t)(0xA0 + i);
    uint8_t blob[20], key[16];
    for (int i = 0; i < 20; i++) blob[i] = plain[i] ^ lepu[i % 16];

    CHECK(oxyii_parse_auth_reply(blob, sizeof(blob), key));
    CHECK(memcmp(key, plain + 4, 16) == 0);
    CHECK(!oxyii_parse_auth_reply(blob, 16, key));   /* the 16-byte non-key reply */
    blob[0] ^= 0x02;                                  /* type 3: not AES */
    CHECK(!oxyii_parse_auth_reply(blob, sizeof(blob), key));
    blob[0] ^= 0x02; blob[1] ^= 0x18;                 /* key length 8 */
    CHECK(!oxyii_parse_auth_reply(blob, sizeof(blob), key));
}

static void test_reply_encrypted(void)
{
    CHECK(oxyii_reply_encrypted(OXYII_OP_LIVE));
    CHECK(oxyii_reply_encrypted(OXYII_OP_GET_INFO));
    CHECK(oxyii_reply_encrypted(OXYII_OP_GET_BATTERY));
    CHECK(oxyii_reply_encrypted(OXYII_OP_FILE_LIST));
    CHECK(oxyii_reply_encrypted(OXYII_OP_FILE_START));
    CHECK(oxyii_reply_encrypted(OXYII_OP_FILE_DATA));
    CHECK(oxyii_reply_encrypted(OXYII_OP_GET_CONFIG));
    CHECK(!oxyii_reply_encrypted(OXYII_OP_AUTH));
    CHECK(!oxyii_reply_encrypted(OXYII_OP_SETUP));
    CHECK(!oxyii_reply_encrypted(OXYII_OP_SET_TIME));
    CHECK(!oxyii_reply_encrypted(OXYII_OP_FILE_END));
}

static void put_slot(uint8_t *p, int i, const char *name)
{
    memset(p + 1 + i * OXYII_NAME_SLOT, 0, OXYII_NAME_SLOT);
    memcpy(p + 1 + i * OXYII_NAME_SLOT, name, strlen(name));
}

static void test_file_list(void)
{
    uint8_t p[1 + 6 * OXYII_NAME_SLOT];
    p[0] = 6;
    put_slot(p, 0, "20261001220000");
    put_slot(p, 1, "20261002220000");
    put_slot(p, 2, "garbage-name!!");
    put_slot(p, 3, "20261003220000");
    put_slot(p, 4, "20261004220000");
    put_slot(p, 5, "20261005220000");

    char names[4][32];
    int total = -1;
    int n = oxyii_parse_file_list(p, sizeof(p), &names[0][0], sizeof(names[0]), 4, &total);
    CHECK(total == 6);
    CHECK(n == 4);
    /* The newest four, oldest of them first, with the extension. */
    CHECK(strcmp(names[0], "20261002220000.o2s") == 0);
    CHECK(strcmp(names[1], "20261003220000.o2s") == 0);
    CHECK(strcmp(names[3], "20261005220000.o2s") == 0);

    /* A count larger than the slots sent reads only what arrived. */
    n = oxyii_parse_file_list(p, 1 + 2 * OXYII_NAME_SLOT, &names[0][0], sizeof(names[0]), 4, &total);
    CHECK(n == 2 && strcmp(names[1], "20261002220000.o2s") == 0);

    uint8_t empty[] = {0};
    CHECK(oxyii_parse_file_list(empty, 1, &names[0][0], sizeof(names[0]), 4, &total) == 0);
    CHECK(total == 0);
}

static void test_file_start(void)
{
    uint8_t p[OXYII_FILE_START_LEN];
    CHECK(oxyii_file_start_payload("20261005220000.o2s", p));
    CHECK(memcmp(p, "20261005220000", 14) == 0);
    for (int i = 14; i < OXYII_FILE_START_LEN; i++) CHECK(p[i] == 0);

    CHECK(oxyii_file_start_payload("20261005220000", p));
    CHECK(!oxyii_file_start_payload("20261005220000.vld", p));
    CHECK(!oxyii_file_start_payload("2026100522000.o2s", p));
    CHECK(!oxyii_file_start_payload("2026100522000x.o2s", p));

    uint8_t o[4];
    oxyii_file_data_payload(0x00012345, o);
    CHECK(o[0] == 0x45 && o[1] == 0x23 && o[2] == 0x01 && o[3] == 0x00);
}

static void test_names(void)
{
    CHECK(oxyii_name_matches("s8-aw 1a2b"));
    CHECK(oxyii_name_matches("t8520_e85a"));
    CHECK(!oxyii_name_matches("o2ring 1276"));
    CHECK(!oxyii_name_matches("s8"));
}

int main(void)
{
    test_crc_fixture();
    test_encode_get_info();
    test_auth_frame();
    test_find_frame();
    test_time();
    test_info();
    test_live();
    test_aes();
    test_auth_reply();
    test_reply_encrypted();
    test_file_list();
    test_file_start();
    test_names();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("oxyii_proto: all passed\n");
    return 0;
}
