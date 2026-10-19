#include "soc.h"
#include "unity.h"

#define CAPACITY_MAH 50000u

static soc_t soc;

void setUp(void)
{
    soc_init(&soc, CAPACITY_MAH, 8000u);
}

void tearDown(void) {}

void test_initial_value(void)
{
    TEST_ASSERT_EQUAL_UINT16(8000, soc_pct_x100(&soc));
}

void test_one_c_discharge_for_six_minutes_removes_ten_percent(void)
{
    /* 50 A for 0.1 h = 5 Ah = 10 % of 50 Ah, integrated in 100 ms steps */
    for (int i = 0; i < 3600; i++) {
        soc_update(&soc, 50000, 100u);
    }
    TEST_ASSERT_EQUAL_UINT16(7000, soc_pct_x100(&soc));
}

void test_charging_increases_and_clamps_at_full(void)
{
    for (int i = 0; i < 10000; i++) {
        soc_update(&soc, -100000, 100u); /* 100 A charge */
    }
    TEST_ASSERT_EQUAL_UINT16(10000, soc_pct_x100(&soc));
}

void test_deep_discharge_clamps_at_empty(void)
{
    for (int i = 0; i < 100000; i++) {
        soc_update(&soc, 300000, 100u);
    }
    TEST_ASSERT_EQUAL_UINT16(0, soc_pct_x100(&soc));
}

void test_small_currents_accumulate_without_rounding_loss(void)
{
    /* 10 mA for 1 h = 10 mAh = 0.02 % of 50 Ah. A naive implementation that
     * converted each 100 ms step to percent would round every step to zero. */
    for (int i = 0; i < 36000; i++) {
        soc_update(&soc, 10, 100u);
    }
    TEST_ASSERT_EQUAL_UINT16(7998, soc_pct_x100(&soc));
}

void test_initial_value_above_100_percent_is_clamped(void)
{
    soc_init(&soc, CAPACITY_MAH, 12000u);
    TEST_ASSERT_EQUAL_UINT16(10000, soc_pct_x100(&soc));
}

void test_zero_capacity_is_safe(void)
{
    soc_init(&soc, 0u, 5000u);
    soc_update(&soc, 1000, 100u);
    TEST_ASSERT_EQUAL_UINT16(0, soc_pct_x100(&soc));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_initial_value);
    RUN_TEST(test_one_c_discharge_for_six_minutes_removes_ten_percent);
    RUN_TEST(test_charging_increases_and_clamps_at_full);
    RUN_TEST(test_deep_discharge_clamps_at_empty);
    RUN_TEST(test_small_currents_accumulate_without_rounding_loss);
    RUN_TEST(test_initial_value_above_100_percent_is_clamped);
    RUN_TEST(test_zero_capacity_is_safe);
    return UNITY_END();
}
