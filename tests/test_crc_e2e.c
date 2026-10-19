#include <string.h>

#include "crc8.h"
#include "e2e.h"
#include "unity.h"

static can_frame_t f;
static e2e_rx_state_t rx;

void setUp(void)
{
    memset(&f, 0, sizeof f);
    f.id = 0x060u;
    f.dlc = 8u;
    f.data[2] = 0x01u;
    rx = (e2e_rx_state_t){ .last_counter = 0u, .synced = false };
}

void tearDown(void) {}

void test_crc8_sae_j1850_check_value(void)
{
    const uint8_t msg[] = "123456789";
    TEST_ASSERT_EQUAL_HEX8(0x4B, crc8_sae_j1850(msg, 9));
}

void test_crc8_of_empty_buffer(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x00, crc8_sae_j1850(NULL, 0));
}

void test_protected_frame_passes_check(void)
{
    uint8_t counter = 5u;
    e2e_protect(&f, &counter);
    TEST_ASSERT_EQUAL_UINT8(6, counter);
    TEST_ASSERT_EQUAL_UINT8(5, f.data[1] & 0x0Fu);
    TEST_ASSERT_EQUAL(E2E_OK, e2e_check(&rx, &f));
}

void test_corrupted_payload_is_detected(void)
{
    uint8_t counter = 0u;
    e2e_protect(&f, &counter);
    f.data[2] ^= 0x04u; /* single bit flip */
    TEST_ASSERT_EQUAL(E2E_ERR_CRC, e2e_check(&rx, &f));
}

void test_frame_under_wrong_id_is_detected(void)
{
    uint8_t counter = 0u;
    e2e_protect(&f, &counter);
    f.id = 0x061u; /* same bytes, different message: Data ID is in the CRC */
    TEST_ASSERT_EQUAL(E2E_ERR_CRC, e2e_check(&rx, &f));
}

void test_repeated_counter_is_rejected(void)
{
    uint8_t counter = 3u;
    e2e_protect(&f, &counter);
    TEST_ASSERT_EQUAL(E2E_OK, e2e_check(&rx, &f));
    TEST_ASSERT_EQUAL(E2E_ERR_REPEATED, e2e_check(&rx, &f)); /* replay */
    e2e_protect(&f, &counter);
    TEST_ASSERT_EQUAL(E2E_OK, e2e_check(&rx, &f));
}

void test_counter_wraps_after_fifteen(void)
{
    uint8_t counter = 15u;
    e2e_protect(&f, &counter);
    TEST_ASSERT_EQUAL_UINT8(0, counter);
    TEST_ASSERT_EQUAL(E2E_OK, e2e_check(&rx, &f));
    e2e_protect(&f, &counter);
    TEST_ASSERT_EQUAL(E2E_OK, e2e_check(&rx, &f));
}

void test_too_short_frame_is_rejected(void)
{
    f.dlc = 1u;
    TEST_ASSERT_EQUAL(E2E_ERR_LENGTH, e2e_check(&rx, &f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc8_sae_j1850_check_value);
    RUN_TEST(test_crc8_of_empty_buffer);
    RUN_TEST(test_protected_frame_passes_check);
    RUN_TEST(test_corrupted_payload_is_detected);
    RUN_TEST(test_frame_under_wrong_id_is_detected);
    RUN_TEST(test_repeated_counter_is_rejected);
    RUN_TEST(test_counter_wraps_after_fifteen);
    RUN_TEST(test_too_short_frame_is_rejected);
    return UNITY_END();
}
