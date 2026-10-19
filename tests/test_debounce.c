#include "debounce.h"
#include "unity.h"

static debounce_t d;

void setUp(void)
{
    debounce_init(&d, 3, 2); /* set after 3 bad samples, clear after 2 good */
}

void tearDown(void) {}

static void feed(bool raw, int n)
{
    for (int i = 0; i < n; i++) {
        (void)debounce_update(&d, raw);
    }
}

void test_starts_inactive(void)
{
    TEST_ASSERT_FALSE(d.active);
}

void test_sets_after_exactly_n_consecutive_samples(void)
{
    TEST_ASSERT_FALSE(debounce_update(&d, true));
    TEST_ASSERT_FALSE(debounce_update(&d, true));
    TEST_ASSERT_TRUE(debounce_update(&d, true));
}

void test_single_glitch_is_ignored(void)
{
    feed(true, 1);
    feed(false, 5);
    TEST_ASSERT_FALSE(d.active);
}

void test_interrupted_run_restarts_the_count(void)
{
    feed(true, 2);
    (void)debounce_update(&d, false); /* one good sample breaks the run */
    feed(true, 2);
    TEST_ASSERT_FALSE(d.active);
    TEST_ASSERT_TRUE(debounce_update(&d, true));
}

void test_clear_requires_m_consecutive_good_samples(void)
{
    feed(true, 3);
    TEST_ASSERT_TRUE(d.active);
    TEST_ASSERT_TRUE(debounce_update(&d, false));
    TEST_ASSERT_FALSE(debounce_update(&d, false));
}

void test_bad_sample_while_clearing_restarts_clear_count(void)
{
    feed(true, 3);
    (void)debounce_update(&d, false);
    (void)debounce_update(&d, true);
    TEST_ASSERT_TRUE(debounce_update(&d, false));
    TEST_ASSERT_FALSE(debounce_update(&d, false));
}

void test_zero_counts_react_on_first_sample(void)
{
    debounce_init(&d, 0, 0);
    TEST_ASSERT_TRUE(debounce_update(&d, true));
    TEST_ASSERT_FALSE(debounce_update(&d, false));
}

void test_above_with_hysteresis(void)
{
    TEST_ASSERT_FALSE(debounce_above(600, 600, 50, false)); /* at the limit is OK */
    TEST_ASSERT_TRUE(debounce_above(601, 600, 50, false));
    TEST_ASSERT_TRUE(debounce_above(560, 600, 50, true));   /* in the band: still bad */
    TEST_ASSERT_FALSE(debounce_above(550, 600, 50, true));
}

void test_below_with_hysteresis(void)
{
    TEST_ASSERT_FALSE(debounce_below(2800, 2800, 200, false));
    TEST_ASSERT_TRUE(debounce_below(2799, 2800, 200, false));
    TEST_ASSERT_TRUE(debounce_below(2900, 2800, 200, true));
    TEST_ASSERT_FALSE(debounce_below(3000, 2800, 200, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_inactive);
    RUN_TEST(test_sets_after_exactly_n_consecutive_samples);
    RUN_TEST(test_single_glitch_is_ignored);
    RUN_TEST(test_interrupted_run_restarts_the_count);
    RUN_TEST(test_clear_requires_m_consecutive_good_samples);
    RUN_TEST(test_bad_sample_while_clearing_restarts_clear_count);
    RUN_TEST(test_zero_counts_react_on_first_sample);
    RUN_TEST(test_above_with_hysteresis);
    RUN_TEST(test_below_with_hysteresis);
    return UNITY_END();
}
