/*
 * SDD-001: host test for the AirSense 11's FIG framing, session cipher and
 * SRP-6a pairing.
 *
 *   cc -I miner/main -I$(brew --prefix mbedtls@3)/include \
 *      miner/test/host/test_as11_proto.c miner/main/as11_fig.c miner/main/as11_srp.c \
 *      -L$(brew --prefix mbedtls@3)/lib -lmbedcrypto -o /tmp/t && /tmp/t
 *
 * Every vector was produced by the protocol's reference implementation
 * (maroliar/hass-resmed-airsense, MIT: srp.py and protocol.py) with fixed
 * random values, so a wrong byte here cannot agree with itself.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "as11_fig.h"
#include "as11_srp.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static size_t hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; s[0] && s[1] && n < cap; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

/* ── the vectors ── */
static const char *A_HEX =
    "630ACDFF5D334462D92A29E0B7FA6E20020F3333292F6D3A640F1C7A76AD9D317531C57979952E5736C88DB118D060DC0539A812B9B0AF3B4002380A9F28AE4A7C45A896542DE05FBCF76A4E7E0739B9A55D5D6C7ABA4F1E1B58729A79BC084D5FF513EAEC33CE978F5BAD87E579B5A95FC773198E22697B2EADAB9EB94F84CDCF1FE94FF09F88D4CA46E968BBA443FF71167571F19FEB052869BD28D7DABF963B7FE399A1F70E7E08D00E1A3778ED1DDDC3325DD09E05D31E774D1FD295C4ABFBC613446232004D67CB03D6A034D2CE6CA0A544A0FF5B434B4B4267FA6C6D72ACBBDA2EFC1EF1D1FE36D35382B089ABE556862AEC35B29D3D0CDF359A9CFED3";
static const char *B_HEX =
    "4A543A4E438654FD009A367BE0D7399605883DCF77D66CA9089F5EE6AA02F4DD8DE04F4AD22717770E94FA03BFDDC76ECA756EAB79832DCCF55DFF0478897243DED17928EB18850BBC1A414B081D690E089D20564F524313BBF7A9BB43A8D5FA20ED1C8353662D35281F69C6DEE2858CDD2CCAC249611A541362CB76C46166A45CBDF2D5BD02820D53915E789FBC50067ABCD4BCFE9B7B18DE6260BB1884E65F35A246E24BB0CB336F0DA1D4CA936A54F8A02E2B882DEBC2F28B98B10FD1B7F26C162C3E2AE1076EAE115D2F563612B7DC8FC322D065E2028893DC3F4690E254DF4B721CAB1B878F51FE5BB8004EC655669C54CB2033BBCD2E02A85ED860D2D0";
static const char *SALT_HEX = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
static const char *M1_HEX = "4D83AB47D53E0B11E7E686CB8E35479E98B2830350CFDDC3A9A963D3318E1EB7";
static const char *M2_HEX = "1EE6ADAE4718D5C203EBE7BE5CAD69FF1E2DAEC0E0D03A6B42EF606FF177F336";
static const char *K_HEX  = "B93068BFDF2B1D98577F696CC022FA049273FD4AC8F42DCC5A3D13E28FF14BC2";
static const char *SESSION_HEX = "67E90B8C4336B3348D882C3D1F1AD07F83A38DBC4597090C89FFDF79F9F503AA";
static const char *HMAC_HEX = "0BAD7C7B0B814A7EF25E2281E1EEC56058B84D251B6345ECD863A61D2BB322FF";
static const char *ENC_HEX =
    "000102030405060708090A0B0C0D0E0F5BED79EA22FA94E1493CF4FCF5C166B9C211685736EB3ED1ECC0CF13D2CC38A2143913D5E44ABCF9B66E22DBBE00BE3FDF379C23BCC075129419E067B044110A43213B33617C0CB05EC416BA60813524";
static const char *FRAME_HEX = "BEBAFECA93030800C5F85D44476E769D7B226964223A317D";

static void test_crc_and_frame(void)
{
    CHECK(as11_crc32((const uint8_t *)"abc", 3) == 0x352441C2u);
    uint8_t out[64];
    size_t n = as11_fig_build(AS11_T_APP_PLAIN, (const uint8_t *)"{\"id\":1}", 8, out, sizeof(out));
    CHECK(n == 24);
    uint8_t want[24];
    hex(FRAME_HEX, want, sizeof(want));
    CHECK(memcmp(out, want, 24) == 0);
    CHECK(as11_fig_build(AS11_T_APP_PLAIN, out, 8, out, 10) == 0);
}

static void test_reassembler(void)
{
    uint8_t f1[64], f2[64];
    size_t n1 = as11_fig_build(AS11_T_DEV_PLAIN, (const uint8_t *)"{\"id\":1}", 8, f1, sizeof(f1));
    size_t n2 = as11_fig_build(AS11_T_DEV_ENC, (const uint8_t *)"0123456789abcdef0123456789abcdef", 32, f2, sizeof(f2));

    as11_reasm_t r;
    as11_reasm_reset(&r);
    uint16_t type; const uint8_t *p; size_t plen;

    /* Split across three notifications, nothing before the end. */
    CHECK(as11_reasm_feed(&r, f1, 5));
    CHECK(!as11_reasm_next(&r, &type, &p, &plen));
    CHECK(as11_reasm_feed(&r, f1 + 5, 10));
    CHECK(!as11_reasm_next(&r, &type, &p, &plen));
    CHECK(as11_reasm_feed(&r, f1 + 15, n1 - 15));
    CHECK(as11_reasm_next(&r, &type, &p, &plen));
    CHECK(type == AS11_T_DEV_PLAIN && plen == 8 && memcmp(p, "{\"id\":1}", 8) == 0);
    CHECK(!as11_reasm_next(&r, &type, &p, &plen));

    /* Two frames in one notification, with noise before the first. */
    uint8_t both[160];
    size_t k = 0;
    both[k++] = 0x11; both[k++] = 0x22;
    memcpy(both + k, f1, n1); k += n1;
    memcpy(both + k, f2, n2); k += n2;
    CHECK(as11_reasm_feed(&r, both, k));
    CHECK(as11_reasm_next(&r, &type, &p, &plen) && type == AS11_T_DEV_PLAIN && plen == 8);
    CHECK(as11_reasm_next(&r, &type, &p, &plen) && type == AS11_T_DEV_ENC && plen == 32);
    CHECK(memcmp(p, "0123456789abcdef", 16) == 0);
    CHECK(!as11_reasm_next(&r, &type, &p, &plen));

    /* A lost notification: the first frame's middle is missing, the next frame
     * starts inside its declared length. The first is dropped, the second kept. */
    k = 0;
    memcpy(both + k, f2, 20); k += 20;           /* header + 4 bytes of f2 only */
    memcpy(both + k, f1, n1); k += n1;
    as11_reasm_reset(&r);
    CHECK(as11_reasm_feed(&r, both, k));
    CHECK(as11_reasm_next(&r, &type, &p, &plen) && type == AS11_T_DEV_PLAIN && plen == 8);
    CHECK(!as11_reasm_next(&r, &type, &p, &plen));

    /* A magic inside a payload is not a frame. */
    uint8_t tricky[64];
    uint8_t body[16] = {0xBE, 0xBA, 0xFE, 0xCA, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    size_t nt = as11_fig_build(AS11_T_DEV_PLAIN, body, 16, tricky, sizeof(tricky));
    as11_reasm_reset(&r);
    CHECK(as11_reasm_feed(&r, tricky, nt));
    CHECK(as11_reasm_next(&r, &type, &p, &plen) && plen == 16 && memcmp(p, body, 16) == 0);
    CHECK(!as11_reasm_next(&r, &type, &p, &plen));
}

static void test_srp(void)
{
    uint8_t a[32];
    hex("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20", a, 32);
    as11_srp_t s;
    CHECK(as11_srp_start(&s, a));
    char ahex[2 * AS11_SRP_N_BYTES + 1];
    as11_hex_encode(s.A, AS11_SRP_N_BYTES, ahex);
    CHECK(strcmp(ahex, A_HEX) == 0);

    char m1[65];
    CHECK(as11_srp_challenge(&s, B_HEX, SALT_HEX, "4829", m1));
    CHECK(strcmp(m1, M1_HEX) == 0);
    char khex[65];
    as11_hex_encode(s.K, 32, khex);
    CHECK(strcmp(khex, K_HEX) == 0);
    CHECK(as11_srp_verify(&s, M2_HEX));
    CHECK(!as11_srp_verify(&s, K_HEX));

    /* A wrong code yields another M1: the machine would refuse it. */
    as11_srp_t w;
    CHECK(as11_srp_start(&w, a));
    char m1w[65];
    CHECK(as11_srp_challenge(&w, B_HEX, SALT_HEX, "4830", m1w));
    CHECK(strcmp(m1w, M1_HEX) != 0);

    /* An invalid server: B = 0 mod N. */
    as11_srp_t z;
    CHECK(as11_srp_start(&z, a));
    CHECK(!as11_srp_challenge(&z, "00", SALT_HEX, "4829", m1w));
    CHECK(!as11_srp_challenge(&z, B_HEX, "zz", "4829", m1w));
}

static void test_session_cipher(void)
{
    uint8_t K[32];
    hex(K_HEX, K, 32);
    uint8_t nonce[32];
    size_t nn = hex("2222222222222222222222222222222222222222222222222222222222222222", nonce, 32);
    as11_cipher_t c;
    as11_cipher_init(&c, K, nonce, nn);
    char shex[65];
    as11_hex_encode(c.key, 32, shex);
    CHECK(strcmp(shex, SESSION_HEX) == 0);

    const char *json = "{\"id\":2,\"jsonrpc\":\"1.0\",\"method\":\"Get\",\"params\":[\"SerialNumber\"]}";
    uint8_t iv[16];
    for (int i = 0; i < 16; i++) iv[i] = (uint8_t)i;
    uint8_t enc[256];
    size_t n = as11_cipher_encrypt(&c, iv, (const uint8_t *)json, strlen(json), enc, sizeof(enc));
    uint8_t want[256];
    size_t wn = hex(ENC_HEX, want, sizeof(want));
    CHECK(n == wn);
    CHECK(n == wn && memcmp(enc, want, n) == 0);

    uint8_t dec[256];
    size_t dn = as11_cipher_decrypt(&c, enc, n, dec, sizeof(dec));
    CHECK(dn == strlen(json));
    CHECK(strcmp((const char *)dec, json) == 0);
    CHECK(as11_cipher_decrypt(&c, enc, 17, dec, sizeof(dec)) == 0);   /* not a block multiple */
    CHECK(as11_cipher_decrypt(&c, enc, n, dec, 4) == 0);              /* does not fit */

    char resp[65];
    CHECK(as11_challenge_response(K, "1111111111111111111111111111111111111111111111111111111111111111", resp));
    CHECK(strcmp(resp, HMAC_HEX) == 0);
    CHECK(!as11_challenge_response(K, "xyz", resp));
}

static void test_base64(void)
{
    uint8_t out[32];
    CHECK(as11_base64_decode("AAECAwQ=", 8, out, sizeof(out)) == 5);
    CHECK(out[0] == 0 && out[4] == 4);
    CHECK(as11_base64_decode("SGVsbG8gd29ybGQ=", 16, out, sizeof(out)) == 11);
    CHECK(memcmp(out, "Hello world", 11) == 0);
    CHECK(as11_base64_decode("!!", 2, out, sizeof(out)) == 0);
    CHECK(as11_base64_decode("SGVsbG8gd29ybGQ=", 16, out, 4) == 0);
}

int main(void)
{
    test_crc_and_frame();
    test_reassembler();
    test_srp();
    test_session_cipher();
    test_base64();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("as11 proto: all checks passed\n");
    return 0;
}
