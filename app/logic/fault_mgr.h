/*
 * fault_mgr.h - Fault detection, debouncing and latching (pure logic).
 *
 * Two bitfields describe the fault picture:
 *   active  - the debounced condition is present *right now*
 *   latched - the fault has tripped and has not been reset yet
 *
 * A fault latches the moment its condition becomes active and stays latched
 * after the condition disappears. Only fault_mgr_try_reset() - driven by an
 * explicit CAN reset command - clears it, and only if the condition is gone.
 * This is deliberate: a BMS must not silently re-close contactors after an
 * intermittent fault; a human or the vehicle controller has to acknowledge it.
 */
#ifndef FAULT_MGR_H
#define FAULT_MGR_H

#include <stddef.h>
#include <stdint.h>

#include "bms_types.h"
#include "debounce.h"

typedef struct {
    debounce_t ov[BMS_NUM_CELLS];
    debounce_t uv[BMS_NUM_CELLS];
    debounce_t ot[BMS_NUM_CELLS];
    debounce_t oc;
    debounce_t meas_timeout;
    uint8_t    external_active; /* conditions reported by other modules (watchdog) */
    uint8_t    active;
    uint8_t    latched;
} fault_mgr_t;

void fault_mgr_init(fault_mgr_t *fm);

/*
 * Evaluate one monitor sample. Writes an event for every fault that latched
 * on this sample into events[] (up to max_events) and returns how many.
 * Measurement-based checks are skipped while the sample is not valid: we
 * cannot judge a cell we have no fresh data for (that is what the
 * measurement-timeout fault is for).
 */
size_t fault_mgr_update(fault_mgr_t *fm, const bms_sample_t *s,
                        bms_fault_event_t *events, size_t max_events);

/* One-shot fault with no lingering condition (e.g. precharge timeout).
 * Returns true if it was newly latched. */
bool fault_mgr_latch(fault_mgr_t *fm, bms_fault_id_t id);

/* Persistent condition owned by another module (e.g. watchdog: "a task is
 * stalled"). Latches on the rising edge. Returns true if newly latched. */
bool fault_mgr_set_external(fault_mgr_t *fm, bms_fault_id_t id, bool active);

/* Clear every latched fault whose condition is no longer active.
 * Returns true if no fault remains latched. */
bool fault_mgr_try_reset(fault_mgr_t *fm);

const char *fault_name(bms_fault_id_t id);

#endif /* FAULT_MGR_H */
