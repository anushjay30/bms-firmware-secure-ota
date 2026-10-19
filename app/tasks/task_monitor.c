/*
 * task_monitor.c - 100 ms cell monitoring: snapshot, validate, integrate SoC.
 */
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "bms_log.h"
#include "bms_tasks.h"
#include "soc.h"

void bms_task_monitor(void *arg)
{
    (void)arg;
    soc_t soc;
    soc_init(&soc, BMS_PACK_CAPACITY_MAH, BMS_SOC_INITIAL_PCT_X100);

    bool ever_valid = false;
    bool was_valid = false;
    uint32_t last_ms = bms_now_ms();
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        /*
         * Why xTaskDelayUntil and not vTaskDelay:
         * vTaskDelay(100) sleeps 100 ms *from now*, so the loop period is
         * 100 ms + however long the body took + any preemption - the sample
         * instants drift later every cycle. xTaskDelayUntil sleeps until an
         * absolute time (last_wake + 100 ms), so samples land on a fixed
         * 100 ms grid regardless of execution time. Fixed-rate sampling is
         * what debounce counts and the SoC integration assume.
         * (vTaskDelayUntil is the older void form of the same call; the x-
         * version also tells us when we missed the deadline.)
         */
        if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BMS_MONITOR_PERIOD_MS)) == pdFALSE) {
            /* Overran: the wake time was already in the past. Re-anchor
             * instead of running a burst of back-to-back catch-up cycles. */
            bms_log("MON", "deadline overrun, re-synchronising");
            last_wake = xTaskGetTickCount();
        }
        bms_test_hook_poll(BMS_TASK_MONITOR);

        const uint32_t now = bms_now_ms();
        meas_store_t snap;
        shared_meas_snapshot(&snap);

        bms_sample_t s;
        meas_build_sample(&snap, now, BMS_MEAS_TIMEOUT_MS, &ever_valid, &s);

        /* Integrate over the *measured* dt, not the nominal period, so a late
         * or skipped cycle does not lose charge. */
        if (s.valid) {
            soc_update(&soc, s.pack_ma, (uint32_t)(now - last_ms));
        }
        last_ms = now;

        shared_publish_sample(&s, soc_pct_x100(&soc));
        /* Length-1 queue + overwrite = "mailbox": the fault task always sees
         * the newest sample and the monitor never blocks on a slow consumer. */
        (void)xQueueOverwrite(g_q_sample, &s);

        if (s.valid != was_valid) {
            if (s.valid) {
                bms_log("MON", "measurements valid: pack %lu mV, SoC %u.%02u %%",
                        (unsigned long)s.pack_mv, soc_pct_x100(&soc) / 100u, soc_pct_x100(&soc) % 100u);
            } else {
                bms_log("MON", "measurements not valid (missing or older than %d ms)", BMS_MEAS_TIMEOUT_MS);
            }
            was_valid = s.valid;
        }

        bms_task_checkin(BMS_TASK_MONITOR);
    }
}
