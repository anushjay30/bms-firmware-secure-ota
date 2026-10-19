#include "meas.h"
#include "unity.h"

#define TIMEOUT_MS 300u

static meas_store_t m;
static bms_sample_t s;
static bool ever_valid;

void setUp(void)
{
    meas_store_init(&m);
    ever_valid = false;
}

void tearDown(void) {}

static void fill_all(uint32_t now)
{
    const uint16_t mv[MEAS_CELLS_PER_PAGE] = { 3700, 3700, 3700 };
    const int16_t dc[MEAS_CELLS_PER_PAGE] = { 250, 250, 250 };
    for (uint8_t p = 0; p < MEAS_PAGES; p++) {
        TEST_ASSERT_TRUE(meas_store_cell_voltages(&m, p, mv, now));
        TEST_ASSERT_TRUE(meas_store_cell_temps(&m, p, dc, now));
    }
    meas_store_pack(&m, 1000, 44000, now);
}

void test_not_valid_before_any_data(void)
{
    meas_build_sample(&m, 100u, TIMEOUT_MS, &ever_valid, &s);
    TEST_ASSERT_FALSE(s.valid);
    TEST_ASSERT_FALSE(s.stale); /* never had data: not a "loss" */
}

void test_not_valid_with_one_page_missing(void)
{
    const uint16_t mv[MEAS_CELLS_PER_PAGE] = { 3700, 3700, 3700 };
    const int16_t dc[MEAS_CELLS_PER_PAGE] = { 250, 250, 250 };
    for (uint8_t p = 0; p < MEAS_PAGES - 1u; p++) {
        (void)meas_store_cell_voltages(&m, p, mv, 0u);
        (void)meas_store_cell_temps(&m, p, dc, 0u);
    }
    meas_store_pack(&m, 0, 0, 0u);
    meas_build_sample(&m, 10u, TIMEOUT_MS, &ever_valid, &s);
    TEST_ASSERT_FALSE(s.valid);
}

void test_valid_when_complete_and_fresh(void)
{
    fill_all(1000u);
    meas_build_sample(&m, 1100u, TIMEOUT_MS, &ever_valid, &s);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_FALSE(s.stale);
    TEST_ASSERT_EQUAL_UINT32(12u * 3700u, s.pack_mv);
    TEST_ASSERT_EQUAL_UINT16(44000, s.link_mv);
}

void test_stale_after_timeout_once_valid(void)
{
    fill_all(1000u);
    meas_build_sample(&m, 1100u, TIMEOUT_MS, &ever_valid, &s);
    meas_build_sample(&m, 1000u + TIMEOUT_MS, TIMEOUT_MS, &ever_valid, &s);
    TEST_ASSERT_TRUE(s.valid); /* exactly at the limit is still fresh */
    meas_build_sample(&m, 1001u + TIMEOUT_MS, TIMEOUT_MS, &ever_valid, &s);
    TEST_ASSERT_FALSE(s.valid);
    TEST_ASSERT_TRUE(s.stale);
}

void test_single_silent_group_makes_data_stale(void)
{
    fill_all(1000u);
    meas_build_sample(&m, 1000u, TIMEOUT_MS, &ever_valid, &s);
    meas_store_pack(&m, 1000, 44000, 1500u); /* only the pack frame keeps coming */
    meas_build_sample(&m, 1500u, TIMEOUT_MS, &ever_valid, &s);
    TEST_ASSERT_TRUE(s.stale);
}

void test_out_of_range_page_is_rejected(void)
{
    const uint16_t mv[MEAS_CELLS_PER_PAGE] = { 1, 2, 3 };
    TEST_ASSERT_FALSE(meas_store_cell_voltages(&m, MEAS_PAGES, mv, 0u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_not_valid_before_any_data);
    RUN_TEST(test_not_valid_with_one_page_missing);
    RUN_TEST(test_valid_when_complete_and_fresh);
    RUN_TEST(test_stale_after_timeout_once_valid);
    RUN_TEST(test_single_silent_group_makes_data_stale);
    RUN_TEST(test_out_of_range_page_is_rejected);
    return UNITY_END();
}
