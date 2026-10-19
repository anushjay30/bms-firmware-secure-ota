/*
 * contactor_sm.h - HV contactor state machine (pure logic).
 *
 *            CLOSE cmd, meas valid           link >= 95% of pack
 *   OPEN ---------------------------> PRECHARGE ------------------> CLOSED
 *    ^  ^          OPEN cmd               |  |                        |
 *    |  +---------------------------------+  | timeout                | OPEN cmd
 *    |  +------------------------------------|------------------------+
 *    |                                       v
 *    +---- RESET cmd && no latched fault -- FAULT <---- any latched fault (from any state)
 *
 * The step function takes time as a parameter instead of reading a clock,
 * so unit tests can drive it through timeouts instantly and deterministically.
 *
 * Update lock: a firmware update may only run while the contactors are
 * open, and they must stay open until it is over. The lock is granted - or
 * refused - here, by the one owner of the contactor state, in the same step
 * that processes CLOSE commands: no other task can check "is it open?" and
 * act on a stale answer. While it is held, CLOSE is refused. The lock never
 * opens anything itself; it only keeps closed what is already open.
 */
#ifndef CONTACTOR_SM_H
#define CONTACTOR_SM_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_types.h"

typedef enum {
    SM_LOCK_NONE = 0,
    SM_LOCK_ACQUIRE,          /* firmware update wants to start            */
    SM_LOCK_RELEASE           /* firmware update session is over           */
} sm_lock_req_t;

typedef struct {
    bms_cmd_t     cmd;            /* command received since the last step     */
    uint8_t       faults_latched; /* from the fault manager (after any reset)  */
    bool          meas_valid;     /* measurements present and fresh            */
    uint32_t      pack_mv;        /* sum of cell voltages                      */
    uint32_t      link_mv;        /* DC-link voltage                           */
    sm_lock_req_t lock_req;       /* update lock request since the last step   */
} sm_inputs_t;

typedef struct {
    bool neg; /* negative main contactor */
    bool pre; /* precharge relay         */
    bool pos; /* positive main contactor */
} contactor_outputs_t;

typedef struct {
    bool        changed;           /* a transition happened on this step      */
    bms_state_t from;
    bms_state_t to;
    bool        rejected;          /* a command was refused (see reason)      */
    bool        precharge_timeout; /* caller must latch BMS_FAULT_PRECHARGE   */
    bool        lock_granted;      /* SM_LOCK_ACQUIRE answered: yes           */
    bool        lock_refused;      /* SM_LOCK_ACQUIRE answered: no (not OPEN) */
    const char *reason;            /* static string for the log               */
} sm_result_t;

typedef struct {
    bms_state_t state;
    uint32_t    entered_ms;   /* time the current state was entered */
    bool        update_lock;  /* firmware update in progress: CLOSE refused */
} contactor_sm_t;

void contactor_sm_init(contactor_sm_t *sm, uint32_t now_ms);

sm_result_t contactor_sm_step(contactor_sm_t *sm, const sm_inputs_t *in, uint32_t now_ms);

/* Coil outputs for a state. FAULT and OPEN are identical on purpose: every
 * relay de-energised is the safe state (contactors are normally-open). */
contactor_outputs_t contactor_sm_outputs(bms_state_t state);

const char *bms_state_name(bms_state_t state);
const char *bms_cmd_name(bms_cmd_t cmd);

#endif /* CONTACTOR_SM_H */
