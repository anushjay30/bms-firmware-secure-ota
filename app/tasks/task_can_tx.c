/*
 * task_can_tx.c - Single owner of CAN transmission.
 *
 * Sends BMS_Status every 100 ms (cyclic) plus immediately on a state change,
 * and BMS_FaultEvent whenever a fault latches. Funnelling every transmit
 * through one task gives one place to order/prioritise traffic and one
 * caller of the driver, so can_if_send() needs no locking.
 */
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "bms_log.h"
#include "bms_tasks.h"
#include "can_if.h"

/* Wrap-safe "now >= deadline" for any width of TickType_t. */
static bool tick_reached(TickType_t now, TickType_t deadline)
{
    return (TickType_t)(now - deadline) < ((TickType_t)~(TickType_t)0 / 2u);
}

static void send(const can_frame_t *f)
{
    if (can_if_send(f) != CAN_IF_OK) {
        static uint32_t s_errors;
        /* Rate-limited: a dead bus must not flood the log queue. */
        if ((s_errors++ % 100u) == 0u) {
            bms_log("TX", "send failed for 0x%03lx (%lu errors)", (unsigned long)f->id, (unsigned long)s_errors);
        }
    }
}

static void send_status(uint8_t *counter)
{
    bms_status_msg_t m;
    shared_get_status(&m);
    can_frame_t f;
    bms_msg_pack_status(&m, counter, &f);
    send(&f);
}

void bms_task_can_tx(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(BMS_STATUS_PERIOD_MS);
    uint8_t status_counter = 0u;
    uint8_t event_counter = 0u;
    TickType_t next = xTaskGetTickCount() + period;

    for (;;) {
        bms_task_checkin(BMS_TASK_CAN_TX);
        bms_test_hook_poll(BMS_TASK_CAN_TX);

        /* Sleep until either a TX request arrives or the next cyclic slot. */
        TickType_t now = xTaskGetTickCount();
        const TickType_t wait = tick_reached(now, next) ? 0u : (TickType_t)(next - now);

        tx_req_t req;
        if (xQueueReceive(g_q_tx, &req, wait) == pdPASS) {
            if (req.type == TX_REQ_FAULT_EVENT) {
                uint8_t latched;
                shared_fault_get(&latched, NULL);
                can_frame_t f;
                bms_msg_pack_fault_event(&req.event, latched, event_counter++, &f);
                send(&f);
            } else if (req.type == TX_REQ_FRAME) {
                send(&req.frame); /* e.g. an update protocol response */
            } else {
                send_status(&status_counter);
            }
        }

        now = xTaskGetTickCount();
        if (tick_reached(now, next)) {
            send_status(&status_counter);
            next += period;               /* fixed grid, like DelayUntil */
            if (tick_reached(now, next)) {
                next = now + period;      /* fell behind: re-anchor */
            }
        }
    }
}
