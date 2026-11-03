/**
 * @file as11_srp.h
 * @brief SRP-6a as the AirSense 11 pairs (SDD-001), on mbedTLS bignums.
 *
 * The first pairing with the code on the machine's screen (PROTOCOL.md §4 of
 * github.com/maroliar/hass-resmed-airsense, MIT):
 *
 *   group RFC 5054 2048-bit, g = 2, H = SHA-256, PAD() = left-pad to 256 bytes,
 *   no identity; the password is the screen code as ASCII
 *   a     32 random bytes,  A = g^a mod N
 *   u     H(PAD(A) | PAD(B)),  k = H(PAD(N) | PAD(g)),  x = H(salt | H(code))
 *   S     (B - k*g^x)^(a + u*x) mod N
 *   K     H(PAD(S))                                    -> masterPairKey
 *   M1    H(H(PAD(N)) xor H(PAD(g)) | salt | PAD(A) | PAD(B) | K)
 *   M2    H(PAD(A) | M1 | K)
 *
 * The miner sends A (StartKeyExchange), gets B and the salt, sends M1
 * (ConfirmKeyExchange), gets M2, the clientId and a nonce; K is kept in NVS
 * for every later session, the code is never stored. A 2048-bit modular
 * exponentiation on an ESP32-C3 takes a few seconds; it runs once per
 * pairing, in the scanner task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AS11_SRP_N_BYTES 256

typedef struct {
    uint8_t a[32];
    uint8_t A[AS11_SRP_N_BYTES];   /* PAD(A), ready to send as hex */
    uint8_t B[AS11_SRP_N_BYTES];   /* PAD(B), once the challenge came */
    uint8_t salt[64];
    size_t  salt_len;
    uint8_t K[32];                 /* the masterPairKey, once the challenge was processed */
    uint8_t M1[32];
    bool    have_A, have_K;
} as11_srp_t;

/* Step 1: A from the caller's 32 random bytes (esp_random on the miner; a
 * fixed value in the host test). False on a bignum failure. */
bool as11_srp_start(as11_srp_t *s, const uint8_t a[32]);

/* Step 2: the machine's serverPk and salt (hex), the screen code; fills K and
 * M1 and writes M1 as upper-case hex (64 chars + NUL). False when B or u is
 * zero mod N (an invalid server), the hex is malformed, or a bignum fails. */
bool as11_srp_challenge(as11_srp_t *s, const char *server_pk_hex, const char *salt_hex,
                        const char *code, char m1_hex[65]);

/* Step 3: true when the machine's serverConfirmation (hex) is M2 for this
 * exchange: the code was right and the machine is the one we talked to. */
bool as11_srp_verify(const as11_srp_t *s, const char *m2_hex);
