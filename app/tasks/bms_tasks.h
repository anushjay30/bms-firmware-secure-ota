/*
 * bms_tasks.h - Internal interfaces between the BMS FreeRTOS tasks.
 *
 * Data flow:
 *
 *   CAN RX --(meas store, critical section)--> Monitor --(sample queue, len 1)--> Fault
 *     |                                           |                                |
 *     +--(command queue)--> Contactor <--(poke)---+------(fault mgr, mutex)--------+
 *                              |                                                   |
 *                              +----(tx request queue)----> CAN TX <---------------+
 *
 *   Supervisor checks every task's watchdog slot; Logger drains the log queue.
 *
 * Tasks are thin wrappers: they gather inputs, call the pure logic in
 * app/logic, and act on the result. All decisions live in app/logic so they
 * can be unit-tested on the host without an RTOS.
 */
#ifndef BMS_TASKS_H
#define BMS_TASKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "bms_config.h"
#include "bms_msgs.h"
#include "bms_types.h"
#include "contactor_sm.h"
#include "meas.h"

/* Watchdog-monitored tasks. Also the TaskId used by BMS_TestHook. */
typedef enum {
    BMS_TASK_CAN_RX    = 0,
    BMS_TASK_MONITOR   = 1,
    BMS_TASK_FAULT     = 2,
    BMS_TASK_CONTACTOR = 3,
    BMS_TASK_CAN_TX    = 4,
    BMS_TASK_LOGGER    = 5,
    BMS_TASK_OTA       = 6, /* only on ports with a bootloader (BMS_PORT_HAS_OTA) */
    BMS_TASK_COUNT
} bms_task_id_t;

/*
 * Priorities (higher number = more urgent; idle task is 0).
 *  - Supervisor highest: it must still run when another task spins forever.
 *  - CAN RX next: it is the "bottom half" of the RX interrupt and must keep
 *    the queue drained.
 *  - Fault + contactor: the safety reaction path.
 *  - Monitor + TX: periodic work with 100 ms deadlines and plenty of slack.
 *  - Update (OTA) below all of them: flash writes and the signature check
 *    are slow, and a firmware transfer must never delay cell monitoring or a
 *    fault reaction - it only gets the CPU time they leave over.
 *  - Logger lowest: diagnostics must never delay control.
 */
#define BMS_PRIO_SUPERVISOR  6
#define BMS_PRIO_CAN_RX      5
#define BMS_PRIO_FAULT       4
#define BMS_PRIO_CONTACTOR   4
#define BMS_PRIO_MONITOR     3
#define BMS_PRIO_CAN_TX      3
#define BMS_PRIO_OTA         2
#define BMS_PRIO_LOGGER      1

/* ---- Task entry points --------------------------------------------------- */
void bms_task_can_rx(void *arg);
void bms_task_monitor(void *arg);
void bms_task_fault(void *arg);
void bms_task_contactor(void *arg);
void bms_task_can_tx(void *arg);
void bms_task_supervisor(void *arg);
void bms_task_ota(void *arg);

const char *bms_task_name(bms_task_id_t id);

/* Log every task's stack high-water mark (startup, and on a debug command). */
void bms_report_stack_usage(void);

/* ---- Time ---------------------------------------------------------------- */
/* Milliseconds since boot as uint32_t. Wraps after ~49.7 days; every
 * comparison in the code uses wrap-safe unsigned subtraction. */
uint32_t bms_now_ms(void);

/* ---- Inter-task channels (created in bms_app.c) ------------------------- */
extern QueueHandle_t g_q_sample; /* monitor -> fault, length 1, overwrite  */
extern QueueHandle_t g_q_cmd;    /* rx/fault/supervisor -> contactor        */
extern QueueHandle_t g_q_tx;     /* any -> CAN TX                           */

typedef enum { TX_REQ_STATUS_NOW, TX_REQ_FAULT_EVENT, TX_REQ_FRAME } tx_req_type_t;
typedef struct {
    tx_req_type_t     type;
    bms_fault_event_t event; /* TX_REQ_FAULT_EVENT */
    can_frame_t       frame; /* TX_REQ_FRAME: sent as is */
} tx_req_t;

void bms_request_status_now(void);
void bms_request_fault_event(const bms_fault_event_t *e);
/* Send a ready-made frame through the CAN TX task (the only caller of
 * can_if_send). Returns false if the TX queue is full. */
bool bms_request_frame(const can_frame_t *f);
/* Wake the contactor task so it reacts now instead of at its next step. */
void bms_poke_contactor(void);

/* ---- Shared state (bms_shared.c) ---------------------------------------- */
void shared_init(void);

/* Measurement store: written by CAN RX, read by monitor/contactor.
 * Guarded by a critical section - the copies are tiny and bounded. */
void shared_meas_put_voltages(uint8_t page, const uint16_t mv[MEAS_CELLS_PER_PAGE], uint32_t now_ms);
void shared_meas_put_temps(uint8_t page, const int16_t dc[MEAS_CELLS_PER_PAGE], uint32_t now_ms);
void shared_meas_put_pack(int32_t pack_ma, uint16_t link_mv, uint32_t now_ms);
void shared_meas_snapshot(meas_store_t *out);

/* Latest monitor results (sample summary + SoC) and contactor state. */
void shared_publish_sample(const bms_sample_t *s, uint16_t soc_pct_x100);
void shared_set_state(bms_state_t state);
void shared_get_sm_meas(uint32_t *pack_mv, uint32_t *link_mv, bool *valid);
void shared_get_status(bms_status_msg_t *msg);

/* Update lock (firmware update interlock, see contactor_sm.h). The update
 * task asks; the contactor task decides inside its state machine step.
 * acquire() blocks up to timeout_ms for the answer: true = granted, the
 * contactors are open and CLOSE is refused until release(). */
bool shared_hv_lock_acquire(uint32_t timeout_ms);
void shared_hv_lock_release(void);
/* Contactor task side: take the pending request, report the answer. */
sm_lock_req_t shared_hv_take_lock_request(void);
void shared_hv_lock_answer(bool granted);

/* Fault manager: guarded by a FreeRTOS mutex (priority inheritance), because
 * three tasks of different priority use it and an update walks every cell. */
size_t shared_fault_update(const bms_sample_t *s, bms_fault_event_t *events, size_t max_events);
bool shared_fault_latch(bms_fault_id_t id);
bool shared_fault_set_external(bms_fault_id_t id, bool active);
bool shared_fault_try_reset(void);
void shared_fault_get(uint8_t *latched, uint8_t *active);

/* Software watchdog. The core tasks are watched from shared_init(); a task
 * that exists only on some ports, or needs a longer deadline, registers
 * itself with bms_task_watch(). */
void bms_task_watch(bms_task_id_t id, uint32_t timeout_ms);
void bms_task_checkin(bms_task_id_t id);
uint32_t shared_wdg_check(uint32_t now_ms, uint32_t ages_ms[BMS_TASK_COUNT]);

/* ---- Firmware update (task_ota.c; no-ops without BMS_PORT_HAS_OTA) ------ */
/* Create the update task's queue (bms_app_init, before any task runs). */
void bms_ota_init(void);
/* Hand a received OTA frame to the update task. Never blocks. */
void bms_ota_post_frame(const can_frame_t *f);
/* True while the running image is on trial and has not confirmed itself. */
bool bms_ota_needs_confirm(void);
/* The supervisor judged the image healthy: make it permanent. */
void bms_ota_request_confirm(void);
/* Test hook: the flash fails a write (or loses power) after `bytes` more
 * programmed bytes. */
void bms_ota_inject_flash_fault(bool power_loss, uint32_t bytes);

/* Test hooks (no-ops unless BMS_ENABLE_TEST_HOOKS). */
void bms_test_hook_request_stall(uint8_t task_id, uint16_t duration_ms);
void bms_test_hook_poll(bms_task_id_t id);

#endif /* BMS_TASKS_H */
