/*
 * task_can_rx.c - Receive and dispatch CAN frames.
 *
 * Decodes measurement frames into the shared store, validates commands
 * (E2E: CRC + alive counter) before forwarding them to the contactor task.
 * Kept short and non-blocking apart from the receive itself, so frames never
 * pile up in the driver queue.
 */
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "bms_hal.h"
#include "bms_log.h"
#include "bms_tasks.h"
#include "can_if.h"
#include "contactor_sm.h"

#if BMS_ENABLE_TEST_HOOKS
static void handle_test_hook(const bms_test_hook_msg_t *h)
{
    switch (h->hook) {
    case BMS_HOOK_STALL_TASK:
        bms_test_hook_request_stall(h->task_id, h->duration_ms);
        break;
    case BMS_HOOK_REPORT_STACKS: {
        bms_report_stack_usage();
        can_if_stats_t s;
        can_if_get_stats(&s);
        bms_log("CAN", "rx %lu frames, %lu overruns, %lu errors; tx %lu frames, %lu errors",
                (unsigned long)s.rx_frames, (unsigned long)s.rx_overruns, (unsigned long)s.rx_errors,
                (unsigned long)s.tx_frames, (unsigned long)s.tx_errors);
        break;
    }
    case BMS_HOOK_REBOOT:
        bms_log("TEST", "reboot requested over CAN");
        bms_log_flush(500u); /* the reset would lose anything still queued */
        hal_system_reset();
        break;
    case BMS_HOOK_FAULT:
        bms_log("TEST", "triggering a deliberate fault");
        bms_log_flush(500u);
        hal_trigger_fault();
        break;
    case BMS_HOOK_FLASH_FAIL_WRITE:
    case BMS_HOOK_FLASH_POWER_LOSS:
        /* Armed by the update task, the flash's only owner. */
        bms_ota_inject_flash_fault(h->hook == BMS_HOOK_FLASH_POWER_LOSS, h->param);
        break;
    case BMS_HOOK_NONE:
    default:
        break;
    }
}
#endif

static void handle_command(const can_frame_t *f, e2e_rx_state_t *e2e)
{
    bms_cmd_t cmd = BMS_CMD_NONE;
    const e2e_result_t r = bms_msg_unpack_command(f, e2e, &cmd);

    if (r != E2E_OK) {
        /* Never act on a command we cannot trust - closing contactors on a
         * corrupted or replayed frame could connect HV unexpectedly. */
        bms_log("RX", "command rejected: %s", e2e_result_name(r));
        return;
    }
    if (cmd == BMS_CMD_NONE) {
        return;
    }
    if (xQueueSend(g_q_cmd, &cmd, 0) != pdPASS) {
        bms_log("RX", "command %s dropped: queue full", bms_cmd_name(cmd));
    }
}

void bms_task_can_rx(void *arg)
{
    (void)arg;
    e2e_rx_state_t cmd_e2e = { .last_counter = 0u, .synced = false };

    for (;;) {
        bms_task_checkin(BMS_TASK_CAN_RX);
        bms_test_hook_poll(BMS_TASK_CAN_RX);

        can_frame_t f;
        /* Bounded wait (not portMAX_DELAY) so the task still checks in with
         * the watchdog on a silent bus. */
        if (can_if_receive(&f, BMS_CAN_RX_TIMEOUT_MS) != CAN_IF_OK) {
            continue;
        }
        const uint32_t now = bms_now_ms();

        switch (f.id) {
        case BMS_ID_CELL_VOLTAGES: {
            uint8_t page;
            uint16_t mv[MEAS_CELLS_PER_PAGE];
            if (bms_msg_unpack_cell_voltages(&f, &page, mv)) {
                shared_meas_put_voltages(page, mv, now);
            }
            break;
        }
        case BMS_ID_CELL_TEMPS: {
            uint8_t page;
            int16_t dc[MEAS_CELLS_PER_PAGE];
            if (bms_msg_unpack_cell_temps(&f, &page, dc)) {
                shared_meas_put_temps(page, dc, now);
            }
            break;
        }
        case BMS_ID_PACK_MEAS: {
            int32_t ma;
            uint16_t link_mv;
            if (bms_msg_unpack_pack_meas(&f, &ma, &link_mv)) {
                shared_meas_put_pack(ma, link_mv, now);
            }
            break;
        }
        case BMS_ID_VCU_COMMAND:
            handle_command(&f, &cmd_e2e);
            break;
        case BMS_ID_OTA_REQUEST:
        case BMS_ID_OTA_DATA:
            /* Just hand it over: flash writes happen in the low-priority
             * update task, never here, so measurement frames keep flowing. */
            bms_ota_post_frame(&f);
            break;
#if BMS_ENABLE_TEST_HOOKS
        case BMS_ID_TEST_HOOK: {
            bms_test_hook_msg_t h;
            if (bms_msg_unpack_test_hook(&f, &h)) {
                handle_test_hook(&h);
            }
            break;
        }
#endif
        default:
            break; /* filtered out by the driver; ignore anything else */
        }
    }
}
