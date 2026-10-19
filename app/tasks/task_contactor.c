/*
 * task_contactor.c - Run the contactor state machine and drive the relays.
 */
#include "FreeRTOS.h"
#include "queue.h"

#include "bms_hal.h"
#include "bms_log.h"
#include "bms_tasks.h"
#include "contactor_sm.h"
#include "fault_mgr.h"

static void apply_outputs(bms_state_t state)
{
    const contactor_outputs_t o = contactor_sm_outputs(state);
    hal_contactors_set(o.neg, o.pre, o.pos);
}

void bms_task_contactor(void *arg)
{
    (void)arg;
    contactor_sm_t sm;
    contactor_sm_init(&sm, bms_now_ms());
    apply_outputs(sm.state);
    shared_set_state(sm.state);

    for (;;) {
        bms_task_checkin(BMS_TASK_CONTACTOR);
        bms_test_hook_poll(BMS_TASK_CONTACTOR);

        /* Wait for a command, but at most one step period: the state machine
         * also needs regular steps to watch precharge progress and timeout. */
        bms_cmd_t cmd = BMS_CMD_NONE;
        (void)xQueueReceive(g_q_cmd, &cmd, pdMS_TO_TICKS(BMS_CONTACTOR_STEP_MS));

        if (cmd != BMS_CMD_NONE) {
            bms_log("SM", "command %s received in %s", bms_cmd_name(cmd), bms_state_name(sm.state));
        }
        if (cmd == BMS_CMD_RESET) {
            /* Clears only faults whose condition is gone; the state machine
             * then decides based on what remains latched. */
            if (!shared_fault_try_reset()) {
                uint8_t latched;
                uint8_t active;
                shared_fault_get(&latched, &active);
                bms_log("SM", "reset: faults still active 0x%02x", (unsigned)active);
            }
        }

        sm_inputs_t in = { .cmd = cmd, .lock_req = shared_hv_take_lock_request() };
        shared_fault_get(&in.faults_latched, NULL);
        shared_get_sm_meas(&in.pack_mv, &in.link_mv, &in.meas_valid);

        const uint32_t now = bms_now_ms();
        const sm_result_t r = contactor_sm_step(&sm, &in, now);

        /* The update lock is decided inside the step, so the answer is
         * consistent with any CLOSE processed in the same step. */
        if (in.lock_req == SM_LOCK_ACQUIRE) {
            shared_hv_lock_answer(r.lock_granted);
            if (r.lock_granted) {
                bms_log("SM", "update lock granted: contactors open, CLOSE refused until the update ends");
            } else {
                bms_log("SM", "update lock refused: contactors not open (%s)", bms_state_name(sm.state));
            }
        } else if (in.lock_req == SM_LOCK_RELEASE) {
            bms_log("SM", "update lock released: CLOSE allowed again");
        }

        if (r.precharge_timeout && shared_fault_latch(BMS_FAULT_PRECHARGE)) {
            const bms_fault_event_t e = { .id = BMS_FAULT_PRECHARGE, .cell = 0u, .value = (int32_t)in.link_mv };
            bms_log("FAULT", "PRECHARGE latched: link %lu mV of pack %lu mV after %d ms",
                    (unsigned long)in.link_mv, (unsigned long)in.pack_mv, BMS_PRECHARGE_TIMEOUT_MS);
            bms_request_fault_event(&e);
        }

        if (r.changed) {
            /* Open/close relays first, then publish: the reported state must
             * never claim "open" while the coils are still energised. */
            apply_outputs(r.to);
            shared_set_state(r.to);
            const contactor_outputs_t o = contactor_sm_outputs(r.to);
            bms_log("SM", "%s -> %s (%s)  NEG=%d PRE=%d POS=%d", bms_state_name(r.from),
                    bms_state_name(r.to), r.reason, o.neg, o.pre, o.pos);
            bms_request_status_now(); /* event-triggered status on top of the 100 ms cycle */
        } else if (r.rejected) {
            bms_log("SM", "%s", r.reason);
        }
    }
}
