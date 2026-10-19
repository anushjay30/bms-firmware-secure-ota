/*
 * Flash HAL semantics (common/src/flash_emul.c): the rules of real NOR flash,
 * and the fault injection the metadata and update tests rely on.
 */
#include <string.h>

#include "flash_hal.h"
#include "unity.h"

#define BASE   0x00010000u              /* flash addresses of the emulated device */
#define PAGES  8u
#define SIZE   (PAGES * FLASH_PAGE_SIZE)

static uint8_t mem[SIZE];
static int power_loss_calls;

static void on_power_loss(void)
{
    power_loss_calls++;
}

static const uint8_t *at(uint32_t addr)
{
    return flash_ptr(addr);
}

static void assert_filled(uint32_t addr, uint32_t len, uint8_t value)
{
    for (uint32_t i = 0; i < len; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(value, at(addr)[i], "unexpected byte value");
    }
}

void setUp(void)
{
    memset(mem, 0x00, sizeof mem); /* "programmed" content everywhere */
    flash_emul_init((uintptr_t)mem, BASE, SIZE);
    power_loss_calls = 0;
}

void tearDown(void) {}

/* ---- erase ---------------------------------------------------------------- */

void test_erase_sets_exactly_the_pages_to_ff(void)
{
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE + FLASH_PAGE_SIZE, 2u * FLASH_PAGE_SIZE));
    assert_filled(BASE, FLASH_PAGE_SIZE, 0x00);
    assert_filled(BASE + FLASH_PAGE_SIZE, 2u * FLASH_PAGE_SIZE, 0xFF);
    assert_filled(BASE + 3u * FLASH_PAGE_SIZE, FLASH_PAGE_SIZE, 0x00);
}

void test_erase_must_be_page_aligned(void)
{
    TEST_ASSERT_EQUAL(FLASH_ERR_ALIGN, flash_erase(BASE + 8u, FLASH_PAGE_SIZE));
    TEST_ASSERT_EQUAL(FLASH_ERR_ALIGN, flash_erase(BASE, FLASH_PAGE_SIZE / 2u));
}

void test_out_of_range_is_rejected(void)
{
    TEST_ASSERT_EQUAL(FLASH_ERR_RANGE, flash_erase(BASE - FLASH_PAGE_SIZE, FLASH_PAGE_SIZE));
    TEST_ASSERT_EQUAL(FLASH_ERR_RANGE, flash_erase(BASE + SIZE, FLASH_PAGE_SIZE));
    TEST_ASSERT_EQUAL(FLASH_ERR_RANGE, flash_erase(BASE + SIZE - FLASH_PAGE_SIZE, 2u * FLASH_PAGE_SIZE));
    const uint8_t d[8] = { 0 };
    TEST_ASSERT_EQUAL(FLASH_ERR_RANGE, flash_program(BASE + SIZE, d, sizeof d));
    TEST_ASSERT_NULL(flash_ptr(BASE + SIZE));
}

/* ---- program ---------------------------------------------------------------- */

void test_program_after_erase_reads_back(void)
{
    const uint8_t d[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE, FLASH_PAGE_SIZE));
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE + 64u, d, sizeof d));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(d, at(BASE + 64u), sizeof d);
    assert_filled(BASE + 80u, 16u, 0xFF); /* neighbours untouched */
}

void test_program_must_be_unit_aligned(void)
{
    const uint8_t d[16] = { 0 };
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE, FLASH_PAGE_SIZE));
    TEST_ASSERT_EQUAL(FLASH_ERR_ALIGN, flash_program(BASE + 4u, d, 8u));  /* address */
    TEST_ASSERT_EQUAL(FLASH_ERR_ALIGN, flash_program(BASE, d, 12u));      /* length  */
    assert_filled(BASE, 16u, 0xFF);
}

void test_program_cannot_set_bits_without_erase(void)
{
    /* mem starts all-zero (programmed): writing 0xFF..would need an erase. */
    const uint8_t ones[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
    TEST_ASSERT_EQUAL(FLASH_ERR_NOT_ERASED, flash_program(BASE, ones, sizeof ones));
    assert_filled(BASE, 8u, 0x00);
}

void test_program_can_only_clear_bits(void)
{
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE, FLASH_PAGE_SIZE));
    const uint8_t f0[8] = { 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0 };
    const uint8_t x0[8] = { 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    const uint8_t x0f[8] = { 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE, f0, 8u));
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE, x0, 8u));             /* clears more bits: fine */
    TEST_ASSERT_EQUAL(FLASH_ERR_NOT_ERASED, flash_program(BASE, x0f, 8u)); /* 0 -> 1: refused  */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(x0, at(BASE), 8u);
}

void test_failed_validation_changes_nothing(void)
{
    /* The second unit is not erasable-compatible: the first must not be written either. */
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE, FLASH_PAGE_SIZE));
    const uint8_t zero8[8] = { 0 };
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE + 8u, zero8, 8u));
    uint8_t d[16];
    memset(d, 0x55, sizeof d);
    TEST_ASSERT_EQUAL(FLASH_ERR_NOT_ERASED, flash_program(BASE, d, sizeof d));
    assert_filled(BASE, 8u, 0xFF);
}

/* ---- write protection ------------------------------------------------------ */

void test_protected_range_refuses_erase_and_program(void)
{
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE, SIZE));
    TEST_ASSERT_TRUE(flash_protect(BASE + 2u * FLASH_PAGE_SIZE, 2u * FLASH_PAGE_SIZE));
    const uint8_t d[8] = { 0 };
    TEST_ASSERT_EQUAL(FLASH_ERR_PROTECTED, flash_erase(BASE + 3u * FLASH_PAGE_SIZE, FLASH_PAGE_SIZE));
    TEST_ASSERT_EQUAL(FLASH_ERR_PROTECTED, flash_erase(BASE, 3u * FLASH_PAGE_SIZE)); /* overlaps */
    TEST_ASSERT_EQUAL(FLASH_ERR_PROTECTED, flash_program(BASE + 2u * FLASH_PAGE_SIZE + 8u, d, 8u));
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE + 4u * FLASH_PAGE_SIZE, d, 8u)); /* next page */
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE + FLASH_PAGE_SIZE, FLASH_PAGE_SIZE));
}

/* ---- fault injection ------------------------------------------------------- */

void test_injected_program_failure_writes_the_prefix_then_fails_once(void)
{
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE, FLASH_PAGE_SIZE));
    uint8_t d[32];
    memset(d, 0x11, sizeof d);
    flash_emul_inject(FLASH_FAULT_PROGRAM_FAIL, 20u);
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE, d, 16u));          /* 16 of the 20 */
    TEST_ASSERT_EQUAL(FLASH_ERR_FAULT, flash_program(BASE + 16u, d, 16u));
    assert_filled(BASE, 20u, 0x11);
    assert_filled(BASE + 20u, 12u, 0xFF);
    /* One-shot: the device works again afterwards. */
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE + 64u, d, 8u));
}

void test_power_loss_stops_mid_write_and_kills_the_device(void)
{
    flash_emul_set_power_loss_handler(on_power_loss);
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE, FLASH_PAGE_SIZE));
    uint8_t d[24];
    memset(d, 0x42, sizeof d);
    flash_emul_inject(FLASH_FAULT_POWER_LOSS, 13u); /* in the middle of a double-word */
    TEST_ASSERT_EQUAL(FLASH_ERR_POWER_LOSS, flash_program(BASE, d, sizeof d));
    TEST_ASSERT_EQUAL(1, power_loss_calls);
    assert_filled(BASE, 13u, 0x42);
    assert_filled(BASE + 13u, 11u, 0xFF);

    /* Dead until power-on: nothing changes. */
    TEST_ASSERT_EQUAL(FLASH_ERR_POWER_LOSS, flash_program(BASE + 64u, d, 8u));
    TEST_ASSERT_EQUAL(FLASH_ERR_POWER_LOSS, flash_erase(BASE, FLASH_PAGE_SIZE));
    assert_filled(BASE, 13u, 0x42);

    flash_emul_power_on();
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE + 64u, d, 8u));
    TEST_ASSERT_EQUAL(1, power_loss_calls);
}

void test_power_loss_during_erase_leaves_a_partly_erased_page(void)
{
    flash_emul_inject(FLASH_FAULT_POWER_LOSS_ERASE, FLASH_PAGE_SIZE + 100u);
    TEST_ASSERT_EQUAL(FLASH_ERR_POWER_LOSS, flash_erase(BASE, 2u * FLASH_PAGE_SIZE));
    assert_filled(BASE, FLASH_PAGE_SIZE + 100u, 0xFF);
    assert_filled(BASE + FLASH_PAGE_SIZE + 100u, FLASH_PAGE_SIZE - 100u, 0x00);
}

void test_program_budget_is_not_consumed_by_erases(void)
{
    flash_emul_inject(FLASH_FAULT_POWER_LOSS, 8u);
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE, 4u * FLASH_PAGE_SIZE));
    const uint8_t d[8] = { 0 };
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE, d, 8u));
    TEST_ASSERT_EQUAL(FLASH_ERR_POWER_LOSS, flash_program(BASE + 8u, d, 8u));
}

void test_statistics_count_programmed_bytes_and_erased_pages(void)
{
    const uint8_t d[16] = { 0 };
    TEST_ASSERT_EQUAL(FLASH_OK, flash_erase(BASE, 3u * FLASH_PAGE_SIZE));
    TEST_ASSERT_EQUAL(FLASH_OK, flash_program(BASE, d, 16u));
    flash_stats_t st;
    flash_emul_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(3u, st.pages_erased);
    TEST_ASSERT_EQUAL_UINT32(16u, st.bytes_programmed);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_erase_sets_exactly_the_pages_to_ff);
    RUN_TEST(test_erase_must_be_page_aligned);
    RUN_TEST(test_out_of_range_is_rejected);
    RUN_TEST(test_program_after_erase_reads_back);
    RUN_TEST(test_program_must_be_unit_aligned);
    RUN_TEST(test_program_cannot_set_bits_without_erase);
    RUN_TEST(test_program_can_only_clear_bits);
    RUN_TEST(test_failed_validation_changes_nothing);
    RUN_TEST(test_protected_range_refuses_erase_and_program);
    RUN_TEST(test_injected_program_failure_writes_the_prefix_then_fails_once);
    RUN_TEST(test_power_loss_stops_mid_write_and_kills_the_device);
    RUN_TEST(test_power_loss_during_erase_leaves_a_partly_erased_page);
    RUN_TEST(test_program_budget_is_not_consumed_by_erases);
    RUN_TEST(test_statistics_count_programmed_bytes_and_erased_pages);
    return UNITY_END();
}
