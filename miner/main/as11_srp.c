#include "as11_srp.h"

#include "as11_fig.h"   /* hex helpers */

#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/sha256.h"

/* RFC 5054, 2048-bit group. */
static const char *AS11_SRP_N_HEX =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CB"
    "B4A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0"
    "CF6095179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740A"
    "DBF4FF747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481"
    "F1D2B9078717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDB"
    "F52FB3786160279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C382"
    "71AE35F8E9DBFBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F"
    "9E4AFF73";

static void sha256(const uint8_t *in, size_t n, uint8_t out[32])
{
    mbedtls_sha256(in, n, out, 0);
}

/* H over several parts. */
typedef struct { const uint8_t *p; size_t n; } part_t;
static void sha256_parts(const part_t *parts, size_t count, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (size_t i = 0; i < count; i++) mbedtls_sha256_update(&c, parts[i].p, parts[i].n);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

/* PAD(x): x as 256 big-endian bytes. False when x does not fit. */
static bool pad(const mbedtls_mpi *x, uint8_t out[AS11_SRP_N_BYTES])
{
    return mbedtls_mpi_write_binary(x, out, AS11_SRP_N_BYTES) == 0;
}

bool as11_srp_start(as11_srp_t *s, const uint8_t a[32])
{
    memset(s, 0, sizeof(*s));
    memcpy(s->a, a, 32);
    mbedtls_mpi N, g, av, A;
    mbedtls_mpi_init(&N); mbedtls_mpi_init(&g); mbedtls_mpi_init(&av); mbedtls_mpi_init(&A);
    bool ok = mbedtls_mpi_read_string(&N, 16, AS11_SRP_N_HEX) == 0 &&
              mbedtls_mpi_lset(&g, 2) == 0 &&
              mbedtls_mpi_read_binary(&av, a, 32) == 0 &&
              mbedtls_mpi_exp_mod(&A, &g, &av, &N, NULL) == 0 &&
              pad(&A, s->A);
    mbedtls_mpi_free(&N); mbedtls_mpi_free(&g); mbedtls_mpi_free(&av); mbedtls_mpi_free(&A);
    s->have_A = ok;
    return ok;
}

bool as11_srp_challenge(as11_srp_t *s, const char *server_pk_hex, const char *salt_hex,
                        const char *code, char m1_hex[65])
{
    if (!s->have_A || !server_pk_hex || !salt_hex || !code) return false;
    if (!as11_hex_decode(salt_hex, s->salt, sizeof(s->salt), &s->salt_len) || s->salt_len == 0)
        return false;

    mbedtls_mpi N, g, B, A, a, u, k, x, gx, kgx, base, exp, ux, S, tmp;
    mbedtls_mpi_init(&N); mbedtls_mpi_init(&g); mbedtls_mpi_init(&B); mbedtls_mpi_init(&A);
    mbedtls_mpi_init(&a); mbedtls_mpi_init(&u); mbedtls_mpi_init(&k); mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&gx); mbedtls_mpi_init(&kgx); mbedtls_mpi_init(&base); mbedtls_mpi_init(&exp);
    mbedtls_mpi_init(&ux); mbedtls_mpi_init(&S); mbedtls_mpi_init(&tmp);

    bool ok = false;
    uint8_t padN[AS11_SRP_N_BYTES], padg[AS11_SRP_N_BYTES], padS[AS11_SRP_N_BYTES];
    uint8_t h[32], hcode[32], hN[32], hg[32], hng[32];

    do {
        if (mbedtls_mpi_read_string(&N, 16, AS11_SRP_N_HEX) != 0) break;
        if (mbedtls_mpi_lset(&g, 2) != 0) break;
        if (mbedtls_mpi_read_string(&B, 16, server_pk_hex) != 0) break;
        /* B mod N must not be zero. */
        if (mbedtls_mpi_mod_mpi(&tmp, &B, &N) != 0 || mbedtls_mpi_cmp_int(&tmp, 0) == 0) break;
        if (!pad(&B, s->B)) break;
        if (mbedtls_mpi_read_binary(&A, s->A, AS11_SRP_N_BYTES) != 0) break;
        if (mbedtls_mpi_read_binary(&a, s->a, 32) != 0) break;
        if (!pad(&N, padN) || !pad(&g, padg)) break;

        /* u = H(PAD(A) | PAD(B)), must not be zero mod N */
        { part_t p[2] = {{s->A, AS11_SRP_N_BYTES}, {s->B, AS11_SRP_N_BYTES}}; sha256_parts(p, 2, h); }
        if (mbedtls_mpi_read_binary(&u, h, 32) != 0 || mbedtls_mpi_cmp_int(&u, 0) == 0) break;
        /* k = H(PAD(N) | PAD(g)) */
        { part_t p[2] = {{padN, AS11_SRP_N_BYTES}, {padg, AS11_SRP_N_BYTES}}; sha256_parts(p, 2, h); }
        if (mbedtls_mpi_read_binary(&k, h, 32) != 0) break;
        /* x = H(salt | H(code)) */
        sha256((const uint8_t *)code, strlen(code), hcode);
        { part_t p[2] = {{s->salt, s->salt_len}, {hcode, 32}}; sha256_parts(p, 2, h); }
        if (mbedtls_mpi_read_binary(&x, h, 32) != 0) break;

        /* S = (B - k*g^x mod N)^(a + u*x) mod N */
        if (mbedtls_mpi_exp_mod(&gx, &g, &x, &N, NULL) != 0) break;
        if (mbedtls_mpi_mul_mpi(&kgx, &k, &gx) != 0) break;
        if (mbedtls_mpi_mod_mpi(&kgx, &kgx, &N) != 0) break;
        if (mbedtls_mpi_sub_mpi(&base, &B, &kgx) != 0) break;
        if (mbedtls_mpi_mod_mpi(&base, &base, &N) != 0) break;     /* non-negative */
        if (mbedtls_mpi_mul_mpi(&ux, &u, &x) != 0) break;
        if (mbedtls_mpi_add_mpi(&exp, &a, &ux) != 0) break;
        if (mbedtls_mpi_exp_mod(&S, &base, &exp, &N, NULL) != 0) break;
        if (!pad(&S, padS)) break;

        /* K = H(PAD(S)) */
        sha256(padS, AS11_SRP_N_BYTES, s->K);
        /* M1 = H(H(PAD(N)) xor H(PAD(g)) | salt | PAD(A) | PAD(B) | K) */
        sha256(padN, AS11_SRP_N_BYTES, hN);
        sha256(padg, AS11_SRP_N_BYTES, hg);
        for (int i = 0; i < 32; i++) hng[i] = hN[i] ^ hg[i];
        {
            part_t p[5] = {{hng, 32}, {s->salt, s->salt_len}, {s->A, AS11_SRP_N_BYTES},
                           {s->B, AS11_SRP_N_BYTES}, {s->K, 32}};
            sha256_parts(p, 5, s->M1);
        }
        as11_hex_encode(s->M1, 32, m1_hex);
        s->have_K = true;
        ok = true;
    } while (0);

    mbedtls_mpi_free(&N); mbedtls_mpi_free(&g); mbedtls_mpi_free(&B); mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&a); mbedtls_mpi_free(&u); mbedtls_mpi_free(&k); mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&gx); mbedtls_mpi_free(&kgx); mbedtls_mpi_free(&base); mbedtls_mpi_free(&exp);
    mbedtls_mpi_free(&ux); mbedtls_mpi_free(&S); mbedtls_mpi_free(&tmp);
    return ok;
}

bool as11_srp_verify(const as11_srp_t *s, const char *m2_hex)
{
    if (!s->have_K || !m2_hex) return false;
    uint8_t given[32];
    size_t n;
    if (!as11_hex_decode(m2_hex, given, sizeof(given), &n) || n != 32) return false;
    uint8_t m2[32];
    part_t p[3] = {{s->A, AS11_SRP_N_BYTES}, {s->M1, 32}, {s->K, 32}};
    sha256_parts(p, 3, m2);
    /* Constant time, for form's sake: a wrong M2 is a wrong machine, not a secret. */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(m2[i] ^ given[i]);
    return diff == 0;
}
