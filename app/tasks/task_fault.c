/*
 * task_fault.c - Run fault detection on every monitor sample.
 */
#include "FreeRTOS.h"
#include "queue.h"

#include "bms_log.h"
#include "bms_tasks.h"
#include "fault_mgr.h"

static void log_fault(const bms_fault_event_t *e)
{
    const char *name = fault_name(e->id);
    switch (e->id) {
    case BMS_FAULT_CELL_OV:
    case BMS_FAULT_CELL_UV:
        bms_log("FAULT", "%s latched: cell %u at %ld mV", name, (unsigned)e->cell, (long)e->value);
        break;
    case BMS_FAULT_CELL_OT:
        bms_log("FAULT", "%s latched: cell %u at %ld.%ld degC", name, (unsigned)e->cell,
                (long)(e->value / 10), (long)(e->value % 10 < 0 ? -(e->value % 10) : e->value % 10));
        break;
    case BMS_FAULT_PACK_OC:
        bms_log("FAULT", "%s latched: pack current %ld mA", name, (long)e->value);
        break;
    case BMS_FAULT_MEAS_TIMEOUT:
        bms_log("FAULT", "%s latched: no fresh data for > %ld ms", name, (long)e->value);
        break;
    default:
        bms_log("FAULT", "%s latched (value %ld)", name, (long)e->value);
        break;
    }
}

void bms_task_fault(void *arg)
{
    (void)arg;

    for (;;) {
        bms_task_checkin(BMS_TASK_FAULT);
        bms_test_hook_poll(BMS_TASK_FAULT);

        bms_sample_t s;
        if (xQueueReceive(g_q_sample, &s, pdMS_TO_TICKS(BMS_MONITOR_PERIOD_MS)) != pdPASS) {
            continue;
        }

        bms_fault_event_t events[BMS_FAULT_COUNT];
        const size_t n = shared_fault_update(&s, events, BMS_FAULT_COUNT);
        for (size_t i = 0; i < n; i++) {
            log_fault(&events[i]);
            bms_request_fault_event(&events[i]);
        }
        if (n > 0u) {
            /* Don't wait for the contactor task's next 10 ms step. */
            bms_poke_contactor();
        }
    }
}
