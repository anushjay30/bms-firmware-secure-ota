/*
 * bms_app.c - Static creation of all RTOS objects.
 *
 * Everything is created with the *Static() API from buffers declared here,
 * before the scheduler starts. Benefits: worst-case RAM is known at link
 * time, creation can never fail at runtime for lack of heap, and there is
 * no fragmentation - the usual expectation for safety-related firmware.
 */
#include "bms_app.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "bms_log.h"
#include "bms_port_config.h"
#include "bms_tasks.h"
#include "can_if.h"

/* pdMS_TO_TICKS() rounds down: a 5 ms timeout on a 10 ms tick would become 0
 * (non-blocking!). Catch any period the port's tick rate cannot represent. */
#define BMS_ASSERT_TICK_MULTIPLE(ms) \
    _Static_assert((ms) % (1000 / configTICK_RATE_HZ) == 0, #ms " must be a multiple of the tick period")
BMS_ASSERT_TICK_MULTIPLE(BMS_MONITOR_PERIOD_MS);
BMS_ASSERT_TICK_MULTIPLE(BMS_STATUS_PERIOD_MS);
BMS_ASSERT_TICK_MULTIPLE(BMS_CONTACTOR_STEP_MS);
BMS_ASSERT_TICK_MULTIPLE(BMS_SUPERVISOR_PERIOD_MS);
BMS_ASSERT_TICK_MULTIPLE(BMS_CAN_RX_TIMEOUT_MS);

/* ---- Queues ---------------------------------------------------------------- */
#define CMD_QUEUE_LEN 8u
#define TX_QUEUE_LEN  16u

QueueHandle_t g_q_sample;
QueueHandle_t g_q_cmd;
QueueHandle_t g_q_tx;

static StaticQueue_t s_q_sample_struct;
static uint8_t s_q_sample_storage[1u * sizeof(bms_sample_t)];
static StaticQueue_t s_q_cmd_struct;
static uint8_t s_q_cmd_storage[CMD_QUEUE_LEN * sizeof(bms_cmd_t)];
static StaticQueue_t s_q_tx_struct;
static uint8_t s_q_tx_storage[TX_QUEUE_LEN * sizeof(tx_req_t)];

/* ---- Tasks ------------------------------------------------------------------ */
/* Stack depth per task, in StackType_t words, from the port's
 * bms_port_config.h. On a microcontroller this is real RAM sized from measured
 * high-water marks (see bms_report_stack_usage); a port that does not care
 * (POSIX, where tasks run on pthread stacks) only defines the default. */
#ifndef BMS_STACK_WORDS_CAN_RX
#define BMS_STACK_WORDS_CAN_RX BMS_STACK_WORDS_DEFAULT
#endif
#ifndef BMS_STACK_WORDS_MONITOR
#define BMS_STACK_WORDS_MONITOR BMS_STACK_WORDS_DEFAULT
#endif
#ifndef BMS_STACK_WORDS_FAULT
#define BMS_STACK_WORDS_FAULT BMS_STACK_WORDS_DEFAULT
#endif
#ifndef BMS_STACK_WORDS_CONTACTOR
#define BMS_STACK_WORDS_CONTACTOR BMS_STACK_WORDS_DEFAULT
#endif
#ifndef BMS_STACK_WORDS_CAN_TX
#define BMS_STACK_WORDS_CAN_TX BMS_STACK_WORDS_DEFAULT
#endif
#ifndef BMS_STACK_WORDS_LOGGER
#define BMS_STACK_WORDS_LOGGER BMS_STACK_WORDS_DEFAULT
#endif
#ifndef BMS_STACK_WORDS_SUPERVISOR
#define BMS_STACK_WORDS_SUPERVISOR BMS_STACK_WORDS_DEFAULT
#endif
#ifndef BMS_STACK_WORDS_OTA
#define BMS_STACK_WORDS_OTA BMS_STACK_WORDS_DEFAULT
#endif
#ifndef BMS_PORT_HAS_REAL_TASK_STACKS
#define BMS_PORT_HAS_REAL_TASK_STACKS 1
#endif
#ifndef BMS_PORT_HAS_OTA
#define BMS_PORT_HAS_OTA 0
#endif

typedef struct {
    TaskFunction_t fn;
    const char    *name;
    UBaseType_t    prio;
    uint32_t       depth;
    StackType_t   *stack;
    StaticTask_t  *tcb;
} task_def_t;

#define DEFINE_TASK_MEM(n, words)            \
    static StackType_t s_stack_##n[words];   \
    static StaticTask_t s_tcb_##n

DEFINE_TASK_MEM(rx, BMS_STACK_WORDS_CAN_RX);
DEFINE_TASK_MEM(mon, BMS_STACK_WORDS_MONITOR);
DEFINE_TASK_MEM(fault, BMS_STACK_WORDS_FAULT);
DEFINE_TASK_MEM(sm, BMS_STACK_WORDS_CONTACTOR);
DEFINE_TASK_MEM(tx, BMS_STACK_WORDS_CAN_TX);
DEFINE_TASK_MEM(log, BMS_STACK_WORDS_LOGGER);
DEFINE_TASK_MEM(sup, BMS_STACK_WORDS_SUPERVISOR);
#if BMS_PORT_HAS_OTA
DEFINE_TASK_MEM(ota, BMS_STACK_WORDS_OTA);
#endif

#define TASK(fn, name, prio, mem, words) { fn, name, prio, words, s_stack_##mem, &s_tcb_##mem }

static const task_def_t s_tasks[] = {
    TASK(bms_task_can_rx,     "can_rx",     BMS_PRIO_CAN_RX,     rx,    BMS_STACK_WORDS_CAN_RX),
    TASK(bms_task_monitor,    "monitor",    BMS_PRIO_MONITOR,    mon,   BMS_STACK_WORDS_MONITOR),
    TASK(bms_task_fault,      "fault",      BMS_PRIO_FAULT,      fault, BMS_STACK_WORDS_FAULT),
    TASK(bms_task_contactor,  "contactor",  BMS_PRIO_CONTACTOR,  sm,    BMS_STACK_WORDS_CONTACTOR),
    TASK(bms_task_can_tx,     "can_tx",     BMS_PRIO_CAN_TX,     tx,    BMS_STACK_WORDS_CAN_TX),
    TASK(bms_logger_task,     "logger",     BMS_PRIO_LOGGER,     log,   BMS_STACK_WORDS_LOGGER),
#if BMS_PORT_HAS_OTA
    TASK(bms_task_ota,        "ota",        BMS_PRIO_OTA,        ota,   BMS_STACK_WORDS_OTA),
#endif
    TASK(bms_task_supervisor, "supervisor", BMS_PRIO_SUPERVISOR, sup,   BMS_STACK_WORDS_SUPERVISOR),
};
#define NUM_TASKS (sizeof s_tasks / sizeof s_tasks[0])

static TaskHandle_t s_handles[NUM_TASKS];

/* Indexed by bms_task_id_t (watchdog slots, BMS_TestHook TaskId). */
static const char *const k_task_names[BMS_TASK_COUNT] = {
    "can_rx", "monitor", "fault", "contactor", "can_tx", "logger", "ota",
};

const char *bms_task_name(bms_task_id_t id)
{
    return ((unsigned)id < (unsigned)BMS_TASK_COUNT) ? k_task_names[id] : "?";
}

void bms_report_stack_usage(void)
{
#if BMS_PORT_HAS_REAL_TASK_STACKS
    /* FreeRTOS fills every stack with 0xA5 at creation; the high-water mark is
     * how many words at the far end have never been overwritten, i.e. the
     * minimum free space since the task started. Size stacks from these
     * numbers plus a margin, after exercising the worst-case paths. */
    for (size_t i = 0; i < NUM_TASKS; i++) {
        const uint32_t depth = s_tasks[i].depth;
        const uint32_t free_words = (uint32_t)uxTaskGetStackHighWaterMark(s_handles[i]);
        const uint32_t used = depth - free_words;
        bms_log("STACK", "%-10s used %4lu of %4lu words (%3lu%%), min free %4lu",
                s_tasks[i].name, (unsigned long)used, (unsigned long)depth,
                (unsigned long)(used * 100u / depth), (unsigned long)free_words);
    }
    const uint32_t idle_free = (uint32_t)uxTaskGetStackHighWaterMark(xTaskGetIdleTaskHandle());
    bms_log("STACK", "%-10s used %4lu of %4lu words", "idle",
            (unsigned long)(configMINIMAL_STACK_SIZE - idle_free), (unsigned long)configMINIMAL_STACK_SIZE);
#else
    bms_log("STACK", "high-water marks are not meaningful on this port (tasks run on host thread stacks)");
#endif
}

/* ---- Requests between tasks ------------------------------------------------ */

void bms_request_status_now(void)
{
    const tx_req_t r = { .type = TX_REQ_STATUS_NOW };
    (void)xQueueSend(g_q_tx, &r, 0);
}

void bms_request_fault_event(const bms_fault_event_t *e)
{
    const tx_req_t r = { .type = TX_REQ_FAULT_EVENT, .event = *e };
    if (xQueueSend(g_q_tx, &r, 0) != pdPASS) {
        bms_log("TX", "fault event %d dropped: TX queue full", (int)e->id);
    }
}

bool bms_request_frame(const can_frame_t *f)
{
    tx_req_t r = { .type = TX_REQ_FRAME };
    r.frame = *f;
    return xQueueSend(g_q_tx, &r, 0) == pdPASS;
}

void bms_poke_contactor(void)
{
    const bms_cmd_t none = BMS_CMD_NONE;
    (void)xQueueSend(g_q_cmd, &none, 0);
}

/* ---- Init -------------------------------------------------------------------- */

bool bms_app_init(const char *can_channel)
{
    /* Only these IDs reach the firmware (kernel/hardware acceptance filter). */
    static const uint32_t accept[] = {
        BMS_ID_VCU_COMMAND, BMS_ID_PACK_MEAS, BMS_ID_CELL_VOLTAGES, BMS_ID_CELL_TEMPS,
#if BMS_ENABLE_TEST_HOOKS
        BMS_ID_TEST_HOOK,
#endif
#if BMS_PORT_HAS_OTA
        BMS_ID_OTA_REQUEST, BMS_ID_OTA_DATA,
#endif
    };

    bms_log_init();
    shared_init();
    bms_ota_init();

    g_q_sample = xQueueCreateStatic(1u, sizeof(bms_sample_t), s_q_sample_storage, &s_q_sample_struct);
    g_q_cmd = xQueueCreateStatic(CMD_QUEUE_LEN, sizeof(bms_cmd_t), s_q_cmd_storage, &s_q_cmd_struct);
    g_q_tx = xQueueCreateStatic(TX_QUEUE_LEN, sizeof(tx_req_t), s_q_tx_storage, &s_q_tx_struct);
    configASSERT(g_q_sample != NULL && g_q_cmd != NULL && g_q_tx != NULL);

    if (can_if_init(can_channel, accept, sizeof accept / sizeof accept[0]) != CAN_IF_OK) {
        bms_log("MAIN", "CAN init failed on '%s'", can_channel);
        return false;
    }

    for (size_t i = 0; i < NUM_TASKS; i++) {
        const task_def_t *t = &s_tasks[i];
        s_handles[i] = xTaskCreateStatic(t->fn, t->name, t->depth, NULL, t->prio, t->stack, t->tcb);
        configASSERT(s_handles[i] != NULL);
    }

    bms_log("MAIN", "%u tasks created, test hooks %s", (unsigned)NUM_TASKS,
            BMS_ENABLE_TEST_HOOKS ? "ENABLED" : "disabled");
    return true;
}
