/*
 * task_supervisor.c - Software watchdog supervisor.
 *
 * Runs at the highest application priority so that a task spinning in an
 * endless loop cannot starve it. Every 50 ms it checks that each task has
 * checked in within its deadline. It is also the judge of whether a newly
 * installed image may confirm itself (health.c).
 */
#include "FreeRTOS.h"
#include "task.h"

#include "bms_hal.h"
#include "bms_log.h"
#include "bms_tasks.h"
#include "health.h"

/* High-water marks are only interesting once every task has gone round its
 * loop a few times (formatting a log line, handling frames, ...). */
#define STARTUP_STACK_REPORT_MS 1000u

/* A trial image confirms itself only after this much continuous health:
 * every task alive, no fault latched or active. */
static void judge_trial_image(health_t *h, uint32_t now, uint32_t stalled)
{
    if (!bms_ota_needs_confirm()) {
        return;
    }
    uint8_t latched;
    uint8_t active;
    shared_fault_get(&latched, &active);
    if (health_update(h, now, stalled == 0u, latched == 0u && active == 0u, BMS_CONFIRM_HEALTHY_MS)) {
        bms_ota_request_confirm();
    }
}

#if defined(BMS_TEST_CRASH_AFTER_MS)
/* Test build only: a "release" that crashes shortly after boot, to show the
 * bootloader rolling it back (sim/tests/test_ota.py). */
static void test_build_crash(uint32_t now)
{
    if (now >= BMS_TEST_CRASH_AFTER_MS) {
        bms_log("TEST", "test build: crashing on purpose");
        bms_log_flush(200u);
        hal_trigger_fault();
    }
}
#endif

void bms_task_supervisor(void *arg)
{
    (void)arg;
    uint32_t prev_stalled = 0u;
    bool ready = false;
    bool stacks_reported = false;
    health_t health;
    health_init(&health);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        (void)xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BMS_SUPERVISOR_PERIOD_MS));

        const uint32_t now = bms_now_ms();
        uint32_t ages[BMS_TASK_COUNT];
        const uint32_t stalled = shared_wdg_check(now, ages);

        if (!ready && stalled == 0u) {
            /* Scheduler running and every task alive: the line CI and the
             * test harness wait for after boot. */
            bms_log("MAIN", "BMS ready");
            ready = true;
        }
        if (ready && !stacks_reported && now >= STARTUP_STACK_REPORT_MS) {
            bms_report_stack_usage();
            stacks_reported = true;
        }
        judge_trial_image(&health, now, stalled);
#if defined(BMS_TEST_CRASH_AFTER_MS)
        test_build_crash(now);
#endif

        if (stalled == prev_stalled) {
            continue;
        }

        for (unsigned i = 0; i < (unsigned)BMS_TASK_COUNT; i++) {
            const uint32_t bit = 1u << i;
            if ((stalled & bit) != 0u && (prev_stalled & bit) == 0u) {
                bms_log("WDG", "task '%s' stalled: no check-in for %lu ms",
                        bms_task_name((bms_task_id_t)i), (unsigned long)ages[i]);
            } else if ((stalled & bit) == 0u && (prev_stalled & bit) != 0u) {
                bms_log("WDG", "task '%s' checking in again", bms_task_name((bms_task_id_t)i));
            }
        }

        if ((stalled & (1u << BMS_TASK_CONTACTOR)) != 0u) {
            /* The task that would normally open the contactors is the one that
             * is stuck, so the supervisor de-energises the coils itself. On
             * hardware the hardware watchdog reset would do the same. */
            hal_contactors_set(false, false, false);
            bms_log("WDG", "contactor task stalled: coils forced open");
        }

        if (shared_fault_set_external(BMS_FAULT_WATCHDOG, stalled != 0u)) {
            const bms_fault_event_t e = { .id = BMS_FAULT_WATCHDOG, .cell = 0u, .value = (int32_t)stalled };
            bms_log("FAULT", "WATCHDOG latched: stalled task mask 0x%02lx", (unsigned long)stalled);
            bms_request_fault_event(&e);
            bms_poke_contactor();
        }
        prev_stalled = stalled;
    }
}
