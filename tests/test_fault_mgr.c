#include "bms_config.h"
#include "fault_mgr.h"
#include "unity.h"

static fault_mgr_t fm;
static bms_sample_t s;
static bms_fault_event_t ev[BMS_FAULT_COUNT];

void setUp(void)
{
    fault_mgr_init(&fm);
    for (unsigned i = 0; i < BMS_NUM_CELLS; i++) {
        s.cell_mv[i] = 3700;
        s.cell_dc[i] = 250;
    }
    s.pack_ma = 10000;
    s.valid = true;
    s.stale = false;
}

void tearDown(void) {}

/* Run n samples and return the total number of events produced. */
static size_t run(int n)
{
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        total += fault_mgr_update(&fm, &s, ev, BMS_FAULT_COUNT);
    }
    return total;
}

void test_nominal_pack_has_no_faults(void)
{
    TEST_ASSERT_EQUAL(0, run(20));
    TEST_ASSERT_EQUAL_HEX8(0, fm.latched);
    TEST_ASSERT_EQUAL_HEX8(0, fm.active);
}

void test_overtemp_latches_after_debounce_and_reports_cell(void)
{
    s.cell_dc[2] = 650; /* cell 3 at 65.0 degC */
    TEST_ASSERT_EQUAL(0, run(BMS_DEBOUNCE_OT_SET - 1));
    TEST_ASSERT_EQUAL(1, fault_mgr_update(&fm, &s, ev, BMS_FAULT_COUNT));
    TEST_ASSERT_EQUAL(BMS_FAULT_CELL_OT, ev[0].id);
    TEST_ASSERT_EQUAL_UINT8(3, ev[0].cell);
    TEST_ASSERT_EQUAL_INT32(650, ev[0].value);
    TEST_ASSERT_EQUAL_HEX8(BMS_FAULT_BIT(BMS_FAULT_CELL_OT), fm.latched);
}

void test_single_sample_spike_does_not_latch(void)
{
    s.pack_ma = 400000;
    (void)run(1);
    s.pack_ma = 10000;
    (void)run(5);
    TEST_ASSERT_EQUAL_HEX8(0, fm.latched);
}

void test_fault_stays_latched_after_condition_clears(void)
{
    s.cell_dc[2] = 650;
    (void)run(BMS_DEBOUNCE_OT_SET);
    s.cell_dc[2] = 250;
    (void)run(BMS_DEBOUNCE_CLEAR + 2);
    TEST_ASSERT_EQUAL_HEX8(0, fm.active);
    TEST_ASSERT_EQUAL_HEX8(BMS_FAULT_BIT(BMS_FAULT_CELL_OT), fm.latched);
}

void test_reset_rejected_while_condition_present(void)
{
    s.cell_dc[2] = 650;
    (void)run(BMS_DEBOUNCE_OT_SET);
    TEST_ASSERT_FALSE(fault_mgr_try_reset(&fm));
    TEST_ASSERT_NOT_EQUAL(0, fm.latched);
}

void test_reset_rejected_inside_hysteresis_band(void)
{
    s.cell_dc[2] = 650;
    (void)run(BMS_DEBOUNCE_OT_SET);
    s.cell_dc[2] = BMS_CELL_OT_DC - 1; /* below the trip limit but above limit - hyst */
    (void)run(BMS_DEBOUNCE_CLEAR + 2);
    TEST_ASSERT_FALSE(fault_mgr_try_reset(&fm));
}

void test_reset_succeeds_once_condition_gone(void)
{
    s.cell_dc[2] = 650;
    (void)run(BMS_DEBOUNCE_OT_SET);
    s.cell_dc[2] = 250;
    (void)run(BMS_DEBOUNCE_CLEAR);
    TEST_ASSERT_TRUE(fault_mgr_try_reset(&fm));
    TEST_ASSERT_EQUAL_HEX8(0, fm.latched);
}

void test_undervoltage_and_overvoltage(void)
{
    s.cell_mv[1] = 2500; /* cell 2 */
    s.cell_mv[7] = 4300; /* cell 8 */
    TEST_ASSERT_EQUAL(2, run(BMS_DEBOUNCE_UV_SET));
    TEST_ASSERT_EQUAL_HEX8(BMS_FAULT_BIT(BMS_FAULT_CELL_UV) | BMS_FAULT_BIT(BMS_FAULT_CELL_OV), fm.latched);
}

void test_event_reports_first_cell_to_trip(void)
{
    s.cell_mv[1] = 2500;
    s.cell_mv[4] = 2600;
    (void)run(BMS_DEBOUNCE_UV_SET - 1);
    TEST_ASSERT_EQUAL(1, fault_mgr_update(&fm, &s, ev, BMS_FAULT_COUNT));
    TEST_ASSERT_EQUAL_UINT8(2, ev[0].cell);
    TEST_ASSERT_EQUAL_INT32(2500, ev[0].value);
}

void test_already_latched_fault_does_not_repeat_event(void)
{
    s.cell_dc[0] = 700;
    (void)run(BMS_DEBOUNCE_OT_SET);
    s.cell_dc[5] = 700; /* a second cell overheats: same fault bit */
    TEST_ASSERT_EQUAL(0, run(BMS_DEBOUNCE_OT_SET + 1));
}

void test_overcurrent_discharge_and_charge_limits(void)
{
    s.pack_ma = BMS_PACK_OC_DISCHARGE_MA; /* exactly at the limit: OK */
    TEST_ASSERT_EQUAL(0, run(5));
    s.pack_ma = -(BMS_PACK_OC_CHARGE_MA + 1);
    TEST_ASSERT_EQUAL(1, run(BMS_DEBOUNCE_OC_SET));
    TEST_ASSERT_EQUAL_INT32(-(BMS_PACK_OC_CHARGE_MA + 1), ev[0].value);
}

void test_invalid_sample_skips_measurement_checks(void)
{
    s.valid = false;
    for (unsigned i = 0; i < BMS_NUM_CELLS; i++) {
        s.cell_mv[i] = 0; /* nothing received yet */
    }
    TEST_ASSERT_EQUAL(0, run(10));
    TEST_ASSERT_EQUAL_HEX8(0, fm.latched);
}

void test_stale_data_latches_measurement_timeout(void)
{
    s.valid = false;
    s.stale = true;
    TEST_ASSERT_EQUAL(1, run(BMS_DEBOUNCE_MEAS_TIMEOUT_SET));
    TEST_ASSERT_EQUAL(BMS_FAULT_MEAS_TIMEOUT, ev[0].id);
}

void test_external_watchdog_condition(void)
{
    TEST_ASSERT_TRUE(fault_mgr_set_external(&fm, BMS_FAULT_WATCHDOG, true));
    TEST_ASSERT_FALSE(fault_mgr_set_external(&fm, BMS_FAULT_WATCHDOG, true)); /* already latched */
    (void)run(3); /* sample updates must not drop the external condition */
    TEST_ASSERT_FALSE(fault_mgr_try_reset(&fm));
    (void)fault_mgr_set_external(&fm, BMS_FAULT_WATCHDOG, false);
    (void)run(1);
    TEST_ASSERT_TRUE(fault_mgr_try_reset(&fm));
}

void test_one_shot_latch_clears_on_reset(void)
{
    TEST_ASSERT_TRUE(fault_mgr_latch(&fm, BMS_FAULT_PRECHARGE));
    TEST_ASSERT_FALSE(fault_mgr_latch(&fm, BMS_FAULT_PRECHARGE));
    TEST_ASSERT_TRUE(fault_mgr_try_reset(&fm));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nominal_pack_has_no_faults);
    RUN_TEST(test_overtemp_latches_after_debounce_and_reports_cell);
    RUN_TEST(test_single_sample_spike_does_not_latch);
    RUN_TEST(test_fault_stays_latched_after_condition_clears);
    RUN_TEST(test_reset_rejected_while_condition_present);
    RUN_TEST(test_reset_rejected_inside_hysteresis_band);
    RUN_TEST(test_reset_succeeds_once_condition_gone);
    RUN_TEST(test_undervoltage_and_overvoltage);
    RUN_TEST(test_event_reports_first_cell_to_trip);
    RUN_TEST(test_already_latched_fault_does_not_repeat_event);
    RUN_TEST(test_overcurrent_discharge_and_charge_limits);
    RUN_TEST(test_invalid_sample_skips_measurement_checks);
    RUN_TEST(test_stale_data_latches_measurement_timeout);
    RUN_TEST(test_external_watchdog_condition);
    RUN_TEST(test_one_shot_latch_clears_on_reset);
    return UNITY_END();
}
