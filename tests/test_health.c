#include "health.h"
#include "unity.h"

#define PERIOD 3000u

static health_t h;

void setUp(void)
{
    health_init(&h);
}

void tearDown(void) {}

void test_confirms_after_a_full_healthy_period(void)
{
    for (uint32_t t = 1000u; t < 1000u + PERIOD; t += 50u) {
        TEST_ASSERT_FALSE(health_update(&h, t, true, true, PERIOD));
    }
    TEST_ASSERT_TRUE(health_update(&h, 1000u + PERIOD, true, true, PERIOD));
}

void test_a_stalled_task_restarts_the_clock(void)
{
    TEST_ASSERT_FALSE(health_update(&h, 0u, true, true, PERIOD));
    TEST_ASSERT_FALSE(health_update(&h, 2900u, false, true, PERIOD)); /* watchdog: a task stalled */
    TEST_ASSERT_FALSE(health_update(&h, 3000u, true, true, PERIOD));
    TEST_ASSERT_FALSE(health_update(&h, 5999u, true, true, PERIOD));
    TEST_ASSERT_TRUE(health_update(&h, 6000u, true, true, PERIOD));
}

void test_a_fault_restarts_the_clock(void)
{
    TEST_ASSERT_FALSE(health_update(&h, 0u, true, true, PERIOD));
    TEST_ASSERT_FALSE(health_update(&h, 2500u, true, false, PERIOD));
    TEST_ASSERT_EQUAL_UINT32(0u, health_healthy_ms(&h, 2500u));
    TEST_ASSERT_FALSE(health_update(&h, 2600u, true, true, PERIOD));
    TEST_ASSERT_FALSE(health_update(&h, 5000u, true, true, PERIOD));
    TEST_ASSERT_EQUAL_UINT32(2400u, health_healthy_ms(&h, 5000u));
}

void test_asks_again_one_period_later(void)
{
    TEST_ASSERT_FALSE(health_update(&h, 0u, true, true, PERIOD));
    TEST_ASSERT_TRUE(health_update(&h, PERIOD, true, true, PERIOD));
    TEST_ASSERT_FALSE(health_update(&h, PERIOD + 100u, true, true, PERIOD));
    TEST_ASSERT_TRUE(health_update(&h, 2u * PERIOD, true, true, PERIOD));
}

void test_works_across_millisecond_counter_wrap(void)
{
    TEST_ASSERT_FALSE(health_update(&h, 0xFFFFFC18u, true, true, PERIOD)); /* 1000 ms before the wrap */
    TEST_ASSERT_FALSE(health_update(&h, 0x000003E7u, true, true, PERIOD)); /* 1999 ms later          */
    TEST_ASSERT_TRUE(health_update(&h, 0x000007D0u, true, true, PERIOD));  /* 3000 ms later          */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_confirms_after_a_full_healthy_period);
    RUN_TEST(test_a_stalled_task_restarts_the_clock);
    RUN_TEST(test_a_fault_restarts_the_clock);
    RUN_TEST(test_asks_again_one_period_later);
    RUN_TEST(test_works_across_millisecond_counter_wrap);
    return UNITY_END();
}
