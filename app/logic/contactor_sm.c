#include "contactor_sm.h"

#include "bms_config.h"

void contactor_sm_init(contactor_sm_t *sm, uint32_t now_ms)
{
    sm->state = BMS_STATE_OPEN;
    sm->entered_ms = now_ms;
    sm->update_lock = false;
}

static void go(contactor_sm_t *sm, sm_result_t *r, bms_state_t to, uint32_t now_ms, const char *reason)
{
    r->changed = true;
    r->from = sm->state;
    r->to = to;
    r->reason = reason;
    sm->state = to;
    sm->entered_ms = now_ms;
}

static void reject(sm_result_t *r, const char *reason)
{
    r->rejected = true;
    r->reason = reason;
}

static bool precharge_complete(const sm_inputs_t *in)
{
    /* Integer form of link >= 95% * pack; no division, no floats. */
    return in->pack_mv > 0u &&
           (uint64_t)in->link_mv * 100u >= (uint64_t)in->pack_mv * (uint64_t)BMS_PRECHARGE_DONE_PCT;
}

/* Answer an update-lock request against the state at the end of this step,
 * so a CLOSE processed in the same step wins and the lock is refused. */
static void apply_lock_request(contactor_sm_t *sm, sm_lock_req_t req, sm_result_t *r)
{
    if (req == SM_LOCK_ACQUIRE) {
        if (sm->state == BMS_STATE_OPEN) {
            sm->update_lock = true;
            r->lock_granted = true;
        } else {
            r->lock_refused = true;
        }
    } else if (req == SM_LOCK_RELEASE) {
        sm->update_lock = false;
    }
}

static void step_state(contactor_sm_t *sm, const sm_inputs_t *in, uint32_t now_ms, sm_result_t *r);

sm_result_t contactor_sm_step(contactor_sm_t *sm, const sm_inputs_t *in, uint32_t now_ms)
{
    sm_result_t r = { .changed = false, .from = sm->state, .to = sm->state, .rejected = false,
                      .precharge_timeout = false, .lock_granted = false, .lock_refused = false, .reason = "" };

    /* Highest priority: any latched fault forces FAULT from every state. */
    if (sm->state != BMS_STATE_FAULT && in->faults_latched != 0u) {
        go(sm, &r, BMS_STATE_FAULT, now_ms, "fault latched");
    } else {
        step_state(sm, in, now_ms, &r);
    }
    apply_lock_request(sm, in->lock_req, &r);
    return r;
}

static void step_state(contactor_sm_t *sm, const sm_inputs_t *in, uint32_t now_ms, sm_result_t *r)
{
    switch (sm->state) {
    case BMS_STATE_OPEN:
        if (in->cmd == BMS_CMD_CLOSE) {
            if (sm->update_lock) {
                /* Connecting HV in the middle of a firmware update would end
                 * in a reboot with the contactors closed. */
                reject(r, "close rejected: firmware update in progress");
            } else if (!in->meas_valid) {
                reject(r, "close rejected: measurements not valid");
            } else {
                go(sm, r, BMS_STATE_PRECHARGE, now_ms, "close command");
            }
        }
        break;

    case BMS_STATE_PRECHARGE:
        if (in->cmd == BMS_CMD_OPEN) {
            go(sm, r, BMS_STATE_OPEN, now_ms, "open command");
        } else if (precharge_complete(in)) {
            go(sm, r, BMS_STATE_CLOSED, now_ms, "precharge complete");
        } else if ((uint32_t)(now_ms - sm->entered_ms) >= (uint32_t)BMS_PRECHARGE_TIMEOUT_MS) {
            /* Unsigned subtraction is wrap-safe: correct even when the
             * millisecond counter rolls over (every ~49.7 days). */
            r->precharge_timeout = true;
            go(sm, r, BMS_STATE_FAULT, now_ms, "precharge timeout");
        }
        break;

    case BMS_STATE_CLOSED:
        if (in->cmd == BMS_CMD_OPEN) {
            /* A production BMS would first ask the VCU to ramp current down:
             * breaking hundreds of amps arcs across the contacts and shortens
             * their life. On a fault we open regardless - safety first. */
            go(sm, r, BMS_STATE_OPEN, now_ms, "open command");
        } else if (in->cmd == BMS_CMD_CLOSE) {
            reject(r, "close ignored: already closed");
        }
        break;

    case BMS_STATE_FAULT:
        if (in->cmd == BMS_CMD_RESET) {
            if (in->faults_latched == 0u) {
                go(sm, r, BMS_STATE_OPEN, now_ms, "fault reset");
            } else {
                reject(r, "reset rejected: fault condition still present");
            }
        } else if (in->cmd == BMS_CMD_CLOSE) {
            reject(r, "close rejected: in FAULT (send reset first)");
        }
        break;

    default:
        go(sm, r, BMS_STATE_FAULT, now_ms, "invalid state");
        break;
    }
}

contactor_outputs_t contactor_sm_outputs(bms_state_t state)
{
    switch (state) {
    case BMS_STATE_PRECHARGE: return (contactor_outputs_t){ .neg = true, .pre = true, .pos = false };
    case BMS_STATE_CLOSED:    return (contactor_outputs_t){ .neg = true, .pre = false, .pos = true };
    case BMS_STATE_OPEN:
    case BMS_STATE_FAULT:
    default:                  return (contactor_outputs_t){ .neg = false, .pre = false, .pos = false };
    }
}

const char *bms_state_name(bms_state_t state)
{
    switch (state) {
    case BMS_STATE_OPEN:      return "OPEN";
    case BMS_STATE_PRECHARGE: return "PRECHARGE";
    case BMS_STATE_CLOSED:    return "CLOSED";
    case BMS_STATE_FAULT:     return "FAULT";
    default:                  return "?";
    }
}

const char *bms_cmd_name(bms_cmd_t cmd)
{
    switch (cmd) {
    case BMS_CMD_NONE:  return "NONE";
    case BMS_CMD_CLOSE: return "CLOSE";
    case BMS_CMD_OPEN:  return "OPEN";
    case BMS_CMD_RESET: return "RESET";
    default:            return "?";
    }
}
