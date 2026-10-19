#include "unity.h"
#include "watchdog.h"

static wdg_t w;

void setUp(void)
{
    wdg_init(&w);
    wdg_register(&w, 0, 300u, 1000u);
    wdg_register(&w, 2, 300u, 1000u);
}

void tearDown(void) {}

void test_tasks_within_deadline_are_healthy(void)
{
    TEST_ASSERT_EQUAL_HEX32(0, wdg_check(&w, 1300u));
}

void test_task_past_deadline_is_reported(void)
{
    wdg_checkin(&w, 0, 1200u);
    TEST_ASSERT_EQUAL_HEX32(1u << 2, wdg_check(&w, 1301u));
    TEST_ASSERT_EQUAL_UINT32(301u, wdg_age_ms(&w, 2, 1301u));
}

void test_checkin_recovers_task(void)
{
    TEST_ASSERT_EQUAL_HEX32((1u << 0) | (1u << 2), wdg_check(&w, 2000u));
    wdg_checkin(&w, 0, 2000u);
    wdg_checkin(&w, 2, 2000u);
    TEST_ASSERT_EQUAL_HEX32(0, wdg_check(&w, 2100u));
}

void test_unregistered_slots_are_ignored(void)
{
    wdg_checkin(&w, 0, 100000u);
    wdg_checkin(&w, 2, 100000u);
    TEST_ASSERT_EQUAL_HEX32(0, wdg_check(&w, 100000u));
}

void test_out_of_range_ids_are_ignored(void)
{
    wdg_register(&w, WDG_MAX_TASKS, 1u, 0u);
    wdg_checkin(&w, WDG_MAX_TASKS + 3u, 0u);
    TEST_ASSERT_EQUAL_HEX32(0, wdg_check(&w, 1100u));
}

void test_ages_are_correct_across_wraparound(void)
{
    wdg_checkin(&w, 0, 0xFFFFFFF0u);
    wdg_checkin(&w, 2, 0xFFFFFFF0u);
    TEST_ASSERT_EQUAL_HEX32(0, wdg_check(&w, 0x00000100u)); /* 272 ms later */
    TEST_ASSERT_EQUAL_UINT32(272u, wdg_age_ms(&w, 0, 0x00000100u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tasks_within_deadline_are_healthy);
    RUN_TEST(test_task_past_deadline_is_reported);
    RUN_TEST(test_checkin_recovers_task);
    RUN_TEST(test_unregistered_slots_are_ignored);
    RUN_TEST(test_out_of_range_ids_are_ignored);
    RUN_TEST(test_ages_are_correct_across_wraparound);
    return UNITY_END();
}
