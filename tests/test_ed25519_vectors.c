/*
 * RFC 8032 section 7.1 Ed25519 test vectors, run through the exact library
 * build the firmware links (Monocypher and its optional Ed25519 module).
 *
 * This pins down that the bootloader verifies standard, SHA-512-based
 * Ed25519 - the algorithm Python's cryptography package signs with - rather
 * than Monocypher's default EdDSA flavour (BLAKE2b-based, incompatible), and
 * that verification rejects any change to the message, the signature or the
 * key, including the classic malleability trick of adding the group order to S.
 */
#include <string.h>

#include "monocypher-ed25519.h"
#include "unity.h"

typedef struct {
    const char *name;
    const char *seed;
    const char *pub;
    const char *msg;
    const char *sig;
} vector_t;

static const vector_t V[] = {
    { "TEST 1",
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
      "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "TEST 2",
      "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
      "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
      "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    { "TEST 3",
      "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
      "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
      "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
      "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    { "TEST SHA(abc)",
      "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
      "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
      "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
      "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
      "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" },
};
#define NUM_VECTORS (sizeof V / sizeof V[0])

/* Order of the Ed25519 base point, L = 2^252 + 27742317777372353535851937790883648493,
 * little-endian. */
static const uint8_t L_ORDER[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

typedef struct {
    uint8_t seed[32];
    uint8_t pub[32];
    uint8_t msg[64];
    size_t  msg_len;
    uint8_t sig[64];
} decoded_t;

static size_t unhex(const char *hex, uint8_t *out, size_t max)
{
    const size_t n = strlen(hex) / 2u;
    TEST_ASSERT_TRUE(n <= max);
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2u; k++) {
            const char c = hex[2u * i + k];
            v = (v << 4) | (unsigned)((c <= '9') ? (c - '0') : (c - 'a' + 10));
        }
        out[i] = (uint8_t)v;
    }
    return n;
}

static decoded_t decode(const vector_t *v)
{
    decoded_t d;
    memset(&d, 0, sizeof d);
    unhex(v->seed, d.seed, sizeof d.seed);
    unhex(v->pub, d.pub, sizeof d.pub);
    d.msg_len = unhex(v->msg, d.msg, sizeof d.msg);
    unhex(v->sig, d.sig, sizeof d.sig);
    return d;
}

void setUp(void) {}
void tearDown(void) {}

void test_public_keys_derive_from_seeds(void)
{
    for (size_t i = 0; i < NUM_VECTORS; i++) {
        decoded_t d = decode(&V[i]);
        uint8_t sk[64], pk[32];
        crypto_ed25519_key_pair(sk, pk, d.seed); /* (wipes the seed) */
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(d.pub, pk, 32, V[i].name);
    }
}

void test_signatures_match_rfc(void)
{
    /* Ed25519 is deterministic: same key + message, same signature. */
    for (size_t i = 0; i < NUM_VECTORS; i++) {
        decoded_t d = decode(&V[i]);
        uint8_t sk[64], pk[32], sig[64];
        crypto_ed25519_key_pair(sk, pk, d.seed);
        crypto_ed25519_sign(sig, sk, d.msg, d.msg_len);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(d.sig, sig, 64, V[i].name);
    }
}

void test_rfc_signatures_verify(void)
{
    for (size_t i = 0; i < NUM_VECTORS; i++) {
        const decoded_t d = decode(&V[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, crypto_ed25519_check(d.sig, d.pub, d.msg, d.msg_len), V[i].name);
    }
}

void test_modified_message_is_rejected(void)
{
    for (size_t i = 0; i < NUM_VECTORS; i++) {
        decoded_t d = decode(&V[i]);
        if (d.msg_len == 0u) {
            d.msg[0] = 0x00; /* empty message: sign "", verify one extra byte */
            TEST_ASSERT_NOT_EQUAL(0, crypto_ed25519_check(d.sig, d.pub, d.msg, 1u));
            continue;
        }
        for (size_t bit = 0; bit < 8u * d.msg_len; bit++) {
            d.msg[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
            TEST_ASSERT_NOT_EQUAL(0, crypto_ed25519_check(d.sig, d.pub, d.msg, d.msg_len));
            d.msg[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        }
    }
}

void test_modified_signature_is_rejected(void)
{
    decoded_t d = decode(&V[2]);
    for (size_t byte = 0; byte < 64u; byte++) { /* R (0..31) and S (32..63) */
        d.sig[byte] ^= 0x01u;
        TEST_ASSERT_NOT_EQUAL(0, crypto_ed25519_check(d.sig, d.pub, d.msg, d.msg_len));
        d.sig[byte] ^= 0x01u;
    }
}

void test_wrong_key_is_rejected(void)
{
    const decoded_t a = decode(&V[1]);
    const decoded_t b = decode(&V[2]);
    TEST_ASSERT_NOT_EQUAL(0, crypto_ed25519_check(a.sig, b.pub, a.msg, a.msg_len));
}

void test_non_canonical_s_is_rejected(void)
{
    /* S + L is congruent to S, so a sloppy verifier accepts it: a second valid
     * signature for the same message (malleability). RFC 8032 requires S < L. */
    decoded_t d = decode(&V[0]);
    unsigned carry = 0;
    for (size_t i = 0; i < 32u; i++) {
        const unsigned sum = (unsigned)d.sig[32u + i] + L_ORDER[i] + carry;
        d.sig[32u + i] = (uint8_t)sum;
        carry = sum >> 8;
    }
    TEST_ASSERT_EQUAL_UINT(0, carry); /* S + L still fits in 32 bytes */
    TEST_ASSERT_NOT_EQUAL(0, crypto_ed25519_check(d.sig, d.pub, d.msg, d.msg_len));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_public_keys_derive_from_seeds);
    RUN_TEST(test_signatures_match_rfc);
    RUN_TEST(test_rfc_signatures_verify);
    RUN_TEST(test_modified_message_is_rejected);
    RUN_TEST(test_modified_signature_is_rejected);
    RUN_TEST(test_wrong_key_is_rejected);
    RUN_TEST(test_non_canonical_s_is_rejected);
    return UNITY_END();
}
