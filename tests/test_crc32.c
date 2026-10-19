#include <string.h>

#include "crc32.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_check_value(void)
{
    /* The catalogue check value of CRC-32/ISO-HDLC (= zlib.crc32). */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32("123456789", 9));
}

void test_empty_input_is_zero(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, crc32("", 0));
}

void test_incremental_equals_one_shot(void)
{
    static const char text[] = "The quick brown fox jumps over the lazy dog";
    const size_t n = strlen(text);
    TEST_ASSERT_EQUAL_HEX32(0x414FA339u, crc32(text, n));
    for (size_t split = 0; split <= n; split++) {
        TEST_ASSERT_EQUAL_HEX32(crc32(text, n), crc32_update(crc32(text, split), text + split, n - split));
    }
}

void test_single_bit_flip_changes_crc(void)
{
    uint8_t buf[256];
    for (size_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)i;
    }
    const uint32_t good = crc32(buf, sizeof buf);
    for (size_t bit = 0; bit < 8u * sizeof buf; bit++) {
        buf[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        TEST_ASSERT_NOT_EQUAL(good, crc32(buf, sizeof buf));
        buf[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_empty_input_is_zero);
    RUN_TEST(test_incremental_equals_one_shot);
    RUN_TEST(test_single_bit_flip_changes_crc);
    return UNITY_END();
}
