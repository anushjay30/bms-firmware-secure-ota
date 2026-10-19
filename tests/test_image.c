/*
 * Image header parsing and verification (common/src/bms_image.c).
 *
 * Builds small signed images in a buffer standing in for a slot, the same
 * way tools/sign_image.py does (CRC-32 of the payload, Ed25519 over
 * SHA-512(header[0..0xBF] || payload)), then attacks them one way at a time.
 */
#include <string.h>

#include "bms_image.h"
#include "crc32.h"
#include "monocypher-ed25519.h"
#include "unity.h"

#define SLOT_BASE    0x00120000u
#define SLOT_SIZE    0x00001000u
#define PAYLOAD_SIZE 600u
#define RAM_START    0x20000000u
#define RAM_END      0x203FF000u

/* Signing keys: the seeds of RFC 8032 TEST 1 (ours) and TEST 2 (foreign). */
static const uint8_t SEED_OURS[32] = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
    0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
};
static const uint8_t SEED_FOREIGN[32] = {
    0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda, 0x9d, 0xb6, 0xc3, 0x46, 0xec, 0x11, 0x4e, 0x0f,
    0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24, 0xda, 0x8c, 0xf6, 0xed, 0x4f, 0xb8, 0xa6, 0xfb,
};

static uint8_t slot[SLOT_SIZE];
static uint8_t pubkey[32];
static image_policy_t policy;

#define ASSERT_RESULT(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(bms_image_result_name(expected), bms_image_result_name(actual))

static void put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static bms_image_header_t *hdr(void)
{
    return (bms_image_header_t *)(void *)slot; /* slot is suitably aligned (static array) */
}

static void restamp_crc(void)
{
    hdr()->image_crc32 = crc32(slot + BMS_IMAGE_HEADER_SIZE, hdr()->image_size);
}

static void sign_with(const uint8_t seed[32])
{
    uint8_t s[32], sk[64], pk[32], digest[64];
    memcpy(s, seed, sizeof s);
    crypto_ed25519_key_pair(sk, pk, s);
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, slot, BMS_IMAGE_SIGNED_HEADER);
    crypto_sha512_update(&ctx, slot + BMS_IMAGE_HEADER_SIZE, hdr()->image_size);
    crypto_sha512_final(&ctx, digest);
    crypto_ed25519_sign(hdr()->signature, sk, digest, sizeof digest);
}

static void build_image(uint32_t version)
{
    memset(slot, 0xFF, sizeof slot); /* erased flash around the image */
    bms_image_header_t h;
    memset(&h, 0, sizeof h);
    h.magic = BMS_IMAGE_MAGIC;
    h.header_size = BMS_IMAGE_HEADER_SIZE;
    h.header_version = BMS_IMAGE_HEADER_VERSION;
    h.image_version = version;
    h.image_size = PAYLOAD_SIZE;
    h.load_address = SLOT_BASE;
    h.vector_table = SLOT_BASE + BMS_IMAGE_HEADER_SIZE;
    memcpy(slot, &h, sizeof h);

    uint8_t *payload = slot + BMS_IMAGE_HEADER_SIZE;
    for (unsigned i = 0; i < PAYLOAD_SIZE; i++) {
        payload[i] = (uint8_t)(i * 7u + 3u);
    }
    put_u32(&payload[0], RAM_END);                                 /* initial MSP          */
    put_u32(&payload[4], SLOT_BASE + BMS_IMAGE_HEADER_SIZE + 0x41u); /* Reset_Handler | Thumb */
    restamp_crc();
    sign_with(SEED_OURS);
}

void setUp(void)
{
    uint8_t s[32], sk[64];
    memcpy(s, SEED_OURS, sizeof s);
    crypto_ed25519_key_pair(sk, pubkey, s);
    policy = (image_policy_t){ .slot_base = SLOT_BASE, .slot_size = SLOT_SIZE, .ram_start = RAM_START,
                               .ram_end = RAM_END, .min_version = 0u, .public_key = pubkey };
    build_image(BMS_VERSION(1, 2, 0));
}

void tearDown(void) {}

/* ---- happy path ------------------------------------------------------------ */

void test_valid_image_passes_and_returns_its_header(void)
{
    bms_image_header_t h;
    ASSERT_RESULT(IMAGE_OK, bms_image_verify(slot, &policy, &h));
    TEST_ASSERT_EQUAL_HEX32(BMS_VERSION(1, 2, 0), h.image_version);
    TEST_ASSERT_EQUAL_UINT32(PAYLOAD_SIZE, h.image_size);
    TEST_ASSERT_EQUAL_HEX32(SLOT_BASE + 0x100u, h.vector_table);
}

/* ---- header ------------------------------------------------------------------ */

void test_erased_slot_is_reported_empty(void)
{
    memset(slot, 0xFF, sizeof slot);
    ASSERT_RESULT(IMAGE_ERR_EMPTY, bms_image_verify(slot, &policy, NULL));
}

void test_unknown_header_layout_is_rejected(void)
{
    hdr()->header_version = BMS_IMAGE_HEADER_VERSION + 1u;
    ASSERT_RESULT(IMAGE_ERR_HEADER, bms_image_verify(slot, &policy, NULL));
}

void test_image_linked_for_the_other_slot_is_rejected(void)
{
    policy.slot_base = 0x00020000u; /* the image says 0x00120000 */
    ASSERT_RESULT(IMAGE_ERR_SLOT, bms_image_verify(slot, &policy, NULL));
}

void test_zero_and_oversized_payloads_are_rejected(void)
{
    hdr()->image_size = 0u;
    ASSERT_RESULT(IMAGE_ERR_SIZE, bms_image_verify(slot, &policy, NULL));
    hdr()->image_size = SLOT_SIZE - BMS_IMAGE_HEADER_SIZE + 1u; /* would read past the slot */
    ASSERT_RESULT(IMAGE_ERR_SIZE, bms_image_verify(slot, &policy, NULL));
}

/* ---- anti-rollback ------------------------------------------------------------ */

void test_version_below_minimum_is_rejected(void)
{
    policy.min_version = BMS_VERSION(1, 2, 1);
    ASSERT_RESULT(IMAGE_ERR_VERSION, bms_image_verify(slot, &policy, NULL));
    policy.min_version = BMS_VERSION(2, 0, 0);
    ASSERT_RESULT(IMAGE_ERR_VERSION, bms_image_verify(slot, &policy, NULL));
}

void test_version_equal_to_minimum_is_accepted(void)
{
    policy.min_version = BMS_VERSION(1, 2, 0);
    ASSERT_RESULT(IMAGE_OK, bms_image_verify(slot, &policy, NULL));
}

void test_raising_the_version_in_the_header_breaks_the_signature(void)
{
    /* The obvious way around anti-rollback: re-label an old image as new. */
    policy.min_version = BMS_VERSION(2, 0, 0);
    hdr()->image_version = BMS_VERSION(9, 0, 0);
    ASSERT_RESULT(IMAGE_ERR_SIGNATURE, bms_image_verify(slot, &policy, NULL));
}

/* ---- integrity and authenticity -------------------------------------------- */

void test_payload_bit_flip_is_caught_by_the_crc(void)
{
    slot[BMS_IMAGE_HEADER_SIZE + 100u] ^= 0x10u;
    ASSERT_RESULT(IMAGE_ERR_CRC, bms_image_verify(slot, &policy, NULL));
}

void test_a_recomputed_crc_does_not_fool_the_signature(void)
{
    /* CRC is not security: an attacker simply fixes it up. */
    slot[BMS_IMAGE_HEADER_SIZE + 100u] ^= 0x10u;
    restamp_crc();
    ASSERT_RESULT(IMAGE_ERR_SIGNATURE, bms_image_verify(slot, &policy, NULL));
}

void test_any_signed_header_byte_is_covered(void)
{
    /* Reserved bytes too: everything in 0x00..0xBF is signed. */
    for (unsigned off = 0x20u; off < BMS_IMAGE_SIGNED_HEADER; off += 0x1Fu) {
        build_image(BMS_VERSION(1, 2, 0));
        slot[off] ^= 0x01u;
        ASSERT_RESULT(IMAGE_ERR_SIGNATURE, bms_image_verify(slot, &policy, NULL));
    }
}

void test_signature_bit_flip_is_rejected(void)
{
    hdr()->signature[10] ^= 0x80u;
    ASSERT_RESULT(IMAGE_ERR_SIGNATURE, bms_image_verify(slot, &policy, NULL));
}

void test_image_signed_with_another_key_is_rejected(void)
{
    sign_with(SEED_FOREIGN);
    ASSERT_RESULT(IMAGE_ERR_SIGNATURE, bms_image_verify(slot, &policy, NULL));
}

void test_missing_public_key_never_skips_the_signature_check(void)
{
    policy.public_key = NULL;
    ASSERT_RESULT(IMAGE_ERR_SIGNATURE, bms_image_verify(slot, &policy, NULL));
}

/* ---- vector table sanity (after the signature: a signed but broken build) --- */

void test_stack_pointer_outside_ram_is_rejected(void)
{
    put_u32(slot + BMS_IMAGE_HEADER_SIZE, RAM_END + 8u);
    restamp_crc();
    sign_with(SEED_OURS);
    ASSERT_RESULT(IMAGE_ERR_VECTORS, bms_image_verify(slot, &policy, NULL));
}

void test_reset_handler_without_thumb_bit_is_rejected(void)
{
    put_u32(slot + BMS_IMAGE_HEADER_SIZE + 4u, SLOT_BASE + BMS_IMAGE_HEADER_SIZE + 0x40u);
    restamp_crc();
    sign_with(SEED_OURS);
    ASSERT_RESULT(IMAGE_ERR_VECTORS, bms_image_verify(slot, &policy, NULL));
}

void test_reset_handler_outside_the_image_is_rejected(void)
{
    put_u32(slot + BMS_IMAGE_HEADER_SIZE + 4u, 0x00020101u); /* into the other slot */
    restamp_crc();
    sign_with(SEED_OURS);
    ASSERT_RESULT(IMAGE_ERR_VECTORS, bms_image_verify(slot, &policy, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid_image_passes_and_returns_its_header);
    RUN_TEST(test_erased_slot_is_reported_empty);
    RUN_TEST(test_unknown_header_layout_is_rejected);
    RUN_TEST(test_image_linked_for_the_other_slot_is_rejected);
    RUN_TEST(test_zero_and_oversized_payloads_are_rejected);
    RUN_TEST(test_version_below_minimum_is_rejected);
    RUN_TEST(test_version_equal_to_minimum_is_accepted);
    RUN_TEST(test_raising_the_version_in_the_header_breaks_the_signature);
    RUN_TEST(test_payload_bit_flip_is_caught_by_the_crc);
    RUN_TEST(test_a_recomputed_crc_does_not_fool_the_signature);
    RUN_TEST(test_any_signed_header_byte_is_covered);
    RUN_TEST(test_signature_bit_flip_is_rejected);
    RUN_TEST(test_image_signed_with_another_key_is_rejected);
    RUN_TEST(test_missing_public_key_never_skips_the_signature_check);
    RUN_TEST(test_stack_pointer_outside_ram_is_rejected);
    RUN_TEST(test_reset_handler_without_thumb_bit_is_rejected);
    RUN_TEST(test_reset_handler_outside_the_image_is_rejected);
    return UNITY_END();
}
