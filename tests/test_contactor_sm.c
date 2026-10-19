#include "bms_config.h"
#include "contactor_sm.h"
#include "unity.h"

#define PACK_MV 44400u

static contactor_sm_t sm;
static sm_inputs_t in;

void setUp(void)
{
    contactor_sm_init(&sm, 1000u);
    in = (sm_inputs_t){ .cmd = BMS_CMD_NONE, .faults_latched = 0u, .meas_valid = true,
                        .pack_mv = PACK_MV, .link_mv = 0u };
}

void tearDown(void) {}

static sm_result_t step(bms_cmd_t cmd, uint32_t now)
{
    in.cmd = cmd;
    return contactor_sm_step(&sm, &in, now);
}

static void reach_closed(void)
{
    (void)step(BMS_CMD_CLOSE, 1000u);
    in.link_mv = PACK_MV;
    (void)step(BMS_CMD_NONE, 1100u);
    TEST_ASSERT_EQUAL(BMS_STATE_CLOSED, sm.state);
}

void test_starts_open_with_all_relays_off(void)
{
    const contactor_outputs_t o = contactor_sm_outputs(sm.state);
    TEST_ASSERT_EQUAL(BMS_STATE_OPEN, sm.state);
    TEST_ASSERT_FALSE(o.neg);
    TEST_ASSERT_FALSE(o.pre);
    TEST_ASSERT_FALSE(o.pos);
}

void test_close_rejected_without_valid_measurements(void)
{
    in.meas_valid = false;
    const sm_result_t r = step(BMS_CMD_CLOSE, 1000u);
    TEST_ASSERT_TRUE(r.rejected);
    TEST_ASSERT_EQUAL(BMS_STATE_OPEN, sm.state);
}

void test_close_enters_precharge_with_neg_and_precharge_relay(void)
{
    const sm_result_t r = step(BMS_CMD_CLOSE, 1000u);
    const contactor_outputs_t o = contactor_sm_outputs(sm.state);
    TEST_ASSERT_TRUE(r.changed);
    TEST_ASSERT_EQUAL(BMS_STATE_PRECHARGE, sm.state);
    TEST_ASSERT_TRUE(o.neg);
    TEST_ASSERT_TRUE(o.pre);
    TEST_ASSERT_FALSE(o.pos);
}

void test_precharge_waits_below_threshold(void)
{
    (void)step(BMS_CMD_CLOSE, 1000u);
    in.link_mv = PACK_MV * 94u / 100u;
    (void)step(BMS_CMD_NONE, 1050u);
    TEST_ASSERT_EQUAL(BMS_STATE_PRECHARGE, sm.state);
}

void test_precharge_completes_at_threshold(void)
{
    (void)step(BMS_CMD_CLOSE, 1000u);
    in.link_mv = PACK_MV * BMS_PRECHARGE_DONE_PCT / 100u;
    (void)step(BMS_CMD_NONE, 1050u);
    const contactor_outputs_t o = contactor_sm_outputs(sm.state);
    TEST_ASSERT_EQUAL(BMS_STATE_CLOSED, sm.state);
    TEST_ASSERT_TRUE(o.neg);
    TEST_ASSERT_FALSE(o.pre); /* precharge path opens once main closes */
    TEST_ASSERT_TRUE(o.pos);
}

void test_precharge_timeout_goes_to_fault(void)
{
    (void)step(BMS_CMD_CLOSE, 1000u);
    in.link_mv = 100u; /* shorted DC link: never charges */
    sm_result_t r = step(BMS_CMD_NONE, 1000u + BMS_PRECHARGE_TIMEOUT_MS - 1u);
    TEST_ASSERT_EQUAL(BMS_STATE_PRECHARGE, sm.state);
    r = step(BMS_CMD_NONE, 1000u + BMS_PRECHARGE_TIMEOUT_MS);
    TEST_ASSERT_TRUE(r.precharge_timeout);
    TEST_ASSERT_EQUAL(BMS_STATE_FAULT, sm.state);
}

void test_precharge_timeout_correct_across_millisecond_wraparound(void)
{
    contactor_sm_init(&sm, 0u);
    (void)step(BMS_CMD_CLOSE, 0xFFFFFF00u); /* 256 ms before the counter wraps */
    sm_result_t r = step(BMS_CMD_NONE, 0x00000010u); /* 272 ms later */
    TEST_ASSERT_FALSE(r.precharge_timeout);
    r = step(BMS_CMD_NONE, 0xFFFFFF00u + BMS_PRECHARGE_TIMEOUT_MS);
    TEST_ASSERT_TRUE(r.precharge_timeout);
}

void test_any_latched_fault_forces_fault_from_closed(void)
{
    reach_closed();
    in.faults_latched = BMS_FAULT_BIT(BMS_FAULT_CELL_OT);
    const sm_result_t r = step(BMS_CMD_NONE, 2000u);
    const contactor_outputs_t o = contactor_sm_outputs(sm.state);
    TEST_ASSERT_TRUE(r.changed);
    TEST_ASSERT_EQUAL(BMS_STATE_FAULT, sm.state);
    TEST_ASSERT_FALSE(o.neg || o.pre || o.pos);
}

void test_fault_does_not_clear_by_itself(void)
{
    in.faults_latched = 1u;
    (void)step(BMS_CMD_NONE, 1000u);
    in.faults_latched = 0u; /* condition gone, but nobody asked */
    (void)step(BMS_CMD_NONE, 5000u);
    TEST_ASSERT_EQUAL(BMS_STATE_FAULT, sm.state);
}

void test_reset_rejected_while_faults_latched(void)
{
    in.faults_latched = 1u;
    (void)step(BMS_CMD_NONE, 1000u);
    const sm_result_t r = step(BMS_CMD_RESET, 1010u);
    TEST_ASSERT_TRUE(r.rejected);
    TEST_ASSERT_EQUAL(BMS_STATE_FAULT, sm.state);
}

void test_reset_returns_to_open_when_faults_cleared(void)
{
    in.faults_latched = 1u;
    (void)step(BMS_CMD_NONE, 1000u);
    in.faults_latched = 0u;
    (void)step(BMS_CMD_RESET, 1010u);
    TEST_ASSERT_EQUAL(BMS_STATE_OPEN, sm.state);
}

void test_close_rejected_in_fault(void)
{
    in.faults_latched = 1u;
    (void)step(BMS_CMD_NONE, 1000u);
    const sm_result_t r = step(BMS_CMD_CLOSE, 1010u);
    TEST_ASSERT_TRUE(r.rejected);
    TEST_ASSERT_EQUAL(BMS_STATE_FAULT, sm.state);
}

void test_open_command_from_closed_and_precharge(void)
{
    reach_closed();
    (void)step(BMS_CMD_OPEN, 2000u);
    TEST_ASSERT_EQUAL(BMS_STATE_OPEN, sm.state);

    in.link_mv = 0u;
    (void)step(BMS_CMD_CLOSE, 3000u);
    TEST_ASSERT_EQUAL(BMS_STATE_PRECHARGE, sm.state);
    (void)step(BMS_CMD_OPEN, 3010u);
    TEST_ASSERT_EQUAL(BMS_STATE_OPEN, sm.state);
}

/* ---- update lock (firmware update interlock) -------------------------------- */

static sm_result_t lock_request(sm_lock_req_t req, uint32_t now)
{
    in.lock_req = req;
    const sm_result_t r = step(BMS_CMD_NONE, now);
    in.lock_req = SM_LOCK_NONE;
    return r;
}

void test_update_lock_is_granted_only_while_open(void)
{
    sm_result_t r = lock_request(SM_LOCK_ACQUIRE, 1000u);
    TEST_ASSERT_TRUE(r.lock_granted);
    TEST_ASSERT_TRUE(sm.update_lock);
    (void)lock_request(SM_LOCK_RELEASE, 1010u);

    (void)step(BMS_CMD_CLOSE, 1020u); /* PRECHARGE */
    r = lock_request(SM_LOCK_ACQUIRE, 1030u);
    TEST_ASSERT_TRUE(r.lock_refused);
    TEST_ASSERT_FALSE(sm.update_lock);

    in.link_mv = PACK_MV;
    (void)step(BMS_CMD_NONE, 1040u); /* CLOSED */
    r = lock_request(SM_LOCK_ACQUIRE, 1050u);
    TEST_ASSERT_TRUE(r.lock_refused);
    TEST_ASSERT_EQUAL(BMS_STATE_CLOSED, sm.state); /* refusing changes nothing */

    in.faults_latched = BMS_FAULT_BIT(BMS_FAULT_CELL_OT);
    (void)step(BMS_CMD_NONE, 1060u); /* FAULT */
    TEST_ASSERT_TRUE(lock_request(SM_LOCK_ACQUIRE, 1070u).lock_refused);
}

void test_close_is_refused_while_an_update_holds_the_lock(void)
{
    (void)lock_request(SM_LOCK_ACQUIRE, 1000u);
    const sm_result_t r = step(BMS_CMD_CLOSE, 1010u);
    TEST_ASSERT_TRUE(r.rejected);
    TEST_ASSERT_EQUAL_STRING("close rejected: firmware update in progress", r.reason);
    TEST_ASSERT_EQUAL(BMS_STATE_OPEN, sm.state);

    (void)lock_request(SM_LOCK_RELEASE, 1020u);
    TEST_ASSERT_TRUE(step(BMS_CMD_CLOSE, 1030u).changed);
    TEST_ASSERT_EQUAL(BMS_STATE_PRECHARGE, sm.state);
}

void test_the_lock_never_opens_anything_itself(void)
{
    reach_closed();
    in.lock_req = SM_LOCK_ACQUIRE;
    const sm_result_t r = step(BMS_CMD_NONE, 1200u);
    TEST_ASSERT_FALSE(r.changed);
    TEST_ASSERT_EQUAL(BMS_STATE_CLOSED, sm.state);
    const contactor_outputs_t o = contactor_sm_outputs(sm.state);
    TEST_ASSERT_TRUE(o.neg && o.pos);
}

void test_close_and_lock_in_the_same_step_close_wins(void)
{
    in.lock_req = SM_LOCK_ACQUIRE;
    const sm_result_t r = step(BMS_CMD_CLOSE, 1000u);
    TEST_ASSERT_TRUE(r.changed);
    TEST_ASSERT_TRUE(r.lock_refused); /* no window where both could be true */
    TEST_ASSERT_FALSE(sm.update_lock);
}

void test_lock_survives_a_fault_and_reset(void)
{
    (void)lock_request(SM_LOCK_ACQUIRE, 1000u);
    in.faults_latched = BMS_FAULT_BIT(BMS_FAULT_CELL_OV);
    (void)step(BMS_CMD_NONE, 1010u);
    in.faults_latched = 0u;
    (void)step(BMS_CMD_RESET, 1020u);
    TEST_ASSERT_EQUAL(BMS_STATE_OPEN, sm.state);
    TEST_ASSERT_TRUE(step(BMS_CMD_CLOSE, 1030u).rejected); /* still updating */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_open_with_all_relays_off);
    RUN_TEST(test_close_rejected_without_valid_measurements);
    RUN_TEST(test_close_enters_precharge_with_neg_and_precharge_relay);
    RUN_TEST(test_precharge_waits_below_threshold);
    RUN_TEST(test_precharge_completes_at_threshold);
    RUN_TEST(test_precharge_timeout_goes_to_fault);
    RUN_TEST(test_precharge_timeout_correct_across_millisecond_wraparound);
    RUN_TEST(test_any_latched_fault_forces_fault_from_closed);
    RUN_TEST(test_fault_does_not_clear_by_itself);
    RUN_TEST(test_reset_rejected_while_faults_latched);
    RUN_TEST(test_reset_returns_to_open_when_faults_cleared);
    RUN_TEST(test_close_rejected_in_fault);
    RUN_TEST(test_open_command_from_closed_and_precharge);
    RUN_TEST(test_update_lock_is_granted_only_while_open);
    RUN_TEST(test_close_is_refused_while_an_update_holds_the_lock);
    RUN_TEST(test_the_lock_never_opens_anything_itself);
    RUN_TEST(test_close_and_lock_in_the_same_step_close_wins);
    RUN_TEST(test_lock_survives_a_fault_and_reset);
    return UNITY_END();
}
