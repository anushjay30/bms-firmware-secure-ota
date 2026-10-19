/*
 * bms_shared.c - State shared between tasks, and how each piece is protected.
 *
 * Rule of thumb used here:
 *  - Short, bounded copies (a few dozen bytes): critical section. Disabling
 *    interrupts/scheduling for microseconds is cheaper than a mutex.
 *  - Longer work touched by tasks of different priority: mutex, which in
 *    FreeRTOS has priority inheritance. If the low-priority holder gets
 *    preempted by a medium task while a high-priority task waits, the holder
 *    is temporarily boosted - avoiding unbounded priority inversion (the bug
 *    that famously reset the Mars Pathfinder lander).
 */
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "bms_log.h"
#include "bms_tasks.h"
#include "fault_mgr.h"
#include "watchdog.h"

static meas_store_t s_meas;

static struct {
    uint32_t    pack_mv;
    bool        valid;
    uint16_t    soc_pct_x100;
    bms_state_t state;
} s_status;

static fault_mgr_t s_fm;
static SemaphoreHandle_t s_fm_mutex;
static StaticSemaphore_t s_fm_mutex_buf;

static wdg_t s_wdg;

static struct {
    sm_lock_req_t request;  /* waiting for the contactor task */
    bool          answered;
    bool          granted;
} s_hv_lock;

#if BMS_ENABLE_TEST_HOOKS
static uint16_t s_stall_ms[BMS_TASK_COUNT];
#endif

uint32_t bms_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void shared_init(void)
{
    meas_store_init(&s_meas);
    s_status.pack_mv = 0u;
    s_status.valid = false;
    s_status.soc_pct_x100 = (uint16_t)BMS_SOC_INITIAL_PCT_X100;
    s_status.state = BMS_STATE_OPEN;

    fault_mgr_init(&s_fm);
    s_fm_mutex = xSemaphoreCreateMutexStatic(&s_fm_mutex_buf);
    configASSERT(s_fm_mutex != NULL);

    wdg_init(&s_wdg);
    for (unsigned i = 0; i <= (unsigned)BMS_TASK_LOGGER; i++) { /* the core tasks */
        wdg_register(&s_wdg, i, BMS_WDG_TIMEOUT_MS, 0u);
    }
}

/* ---- Measurements -------------------------------------------------------- */

void shared_meas_put_voltages(uint8_t page, const uint16_t mv[MEAS_CELLS_PER_PAGE], uint32_t now_ms)
{
    taskENTER_CRITICAL();
    (void)meas_store_cell_voltages(&s_meas, page, mv, now_ms);
    taskEXIT_CRITICAL();
}

void shared_meas_put_temps(uint8_t page, const int16_t dc[MEAS_CELLS_PER_PAGE], uint32_t now_ms)
{
    taskENTER_CRITICAL();
    (void)meas_store_cell_temps(&s_meas, page, dc, now_ms);
    taskEXIT_CRITICAL();
}

void shared_meas_put_pack(int32_t pack_ma, uint16_t link_mv, uint32_t now_ms)
{
    taskENTER_CRITICAL();
    meas_store_pack(&s_meas, pack_ma, link_mv, now_ms);
    taskEXIT_CRITICAL();
}

void shared_meas_snapshot(meas_store_t *out)
{
    /* Copying under the lock gives the monitor a *coherent* view: it can
     * never see page 1 from one frame and page 2 from a half-written one. */
    taskENTER_CRITICAL();
    *out = s_meas;
    taskEXIT_CRITICAL();
}

/* ---- Status -------------------------------------------------------------- */

void shared_publish_sample(const bms_sample_t *s, uint16_t soc_pct_x100)
{
    taskENTER_CRITICAL();
    s_status.pack_mv = s->pack_mv;
    s_status.valid = s->valid;
    s_status.soc_pct_x100 = soc_pct_x100;
    taskEXIT_CRITICAL();
}

void shared_set_state(bms_state_t state)
{
    taskENTER_CRITICAL();
    s_status.state = state;
    taskEXIT_CRITICAL();
}

void shared_get_sm_meas(uint32_t *pack_mv, uint32_t *link_mv, bool *valid)
{
    taskENTER_CRITICAL();
    *pack_mv = s_status.pack_mv;
    *valid = s_status.valid;
    /* Link voltage straight from the store (10 ms updates) so precharge
     * completion is seen promptly, not only at the 100 ms monitor rate. */
    *link_mv = s_meas.link_mv;
    taskEXIT_CRITICAL();
}

void shared_get_status(bms_status_msg_t *msg)
{
    uint8_t latched;
    uint8_t active;
    shared_fault_get(&latched, &active);

    taskENTER_CRITICAL();
    msg->state = s_status.state;
    msg->meas_valid = s_status.valid;
    msg->soc_pct_x100 = s_status.soc_pct_x100;
    msg->pack_mv = (s_status.pack_mv > 0xFFFFu) ? 0xFFFFu : (uint16_t)s_status.pack_mv;
    taskEXIT_CRITICAL();

    msg->faults_latched = latched;
    msg->faults_active = active;
}

/* ---- Update lock ----------------------------------------------------------- */

bool shared_hv_lock_acquire(uint32_t timeout_ms)
{
    taskENTER_CRITICAL();
    s_hv_lock.request = SM_LOCK_ACQUIRE;
    s_hv_lock.answered = false;
    taskEXIT_CRITICAL();
    bms_poke_contactor(); /* decide now, not at the next 10 ms step */

    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    for (TickType_t waited = 0; waited <= limit; waited++) {
        taskENTER_CRITICAL();
        const bool answered = s_hv_lock.answered;
        const bool granted = s_hv_lock.granted;
        taskEXIT_CRITICAL();
        if (answered) {
            return granted;
        }
        vTaskDelay(1);
    }
    /* No answer (contactor task stalled): never assume yes. Withdraw the
     * request so a late "granted" cannot leave a lock nobody owns. */
    shared_hv_lock_release();
    return false;
}

void shared_hv_lock_release(void)
{
    taskENTER_CRITICAL();
    s_hv_lock.request = SM_LOCK_RELEASE;
    s_hv_lock.answered = false;
    taskEXIT_CRITICAL();
    bms_poke_contactor();
}

sm_lock_req_t shared_hv_take_lock_request(void)
{
    taskENTER_CRITICAL();
    const sm_lock_req_t req = s_hv_lock.request;
    s_hv_lock.request = SM_LOCK_NONE;
    taskEXIT_CRITICAL();
    return req;
}

void shared_hv_lock_answer(bool granted)
{
    taskENTER_CRITICAL();
    s_hv_lock.granted = granted;
    s_hv_lock.answered = true;
    taskEXIT_CRITICAL();
}

/* ---- Fault manager ------------------------------------------------------- */

size_t shared_fault_update(const bms_sample_t *s, bms_fault_event_t *events, size_t max_events)
{
    xSemaphoreTake(s_fm_mutex, portMAX_DELAY);
    const size_t n = fault_mgr_update(&s_fm, s, events, max_events);
    xSemaphoreGive(s_fm_mutex);
    return n;
}

bool shared_fault_latch(bms_fault_id_t id)
{
    xSemaphoreTake(s_fm_mutex, portMAX_DELAY);
    const bool newly = fault_mgr_latch(&s_fm, id);
    xSemaphoreGive(s_fm_mutex);
    return newly;
}

bool shared_fault_set_external(bms_fault_id_t id, bool active)
{
    xSemaphoreTake(s_fm_mutex, portMAX_DELAY);
    const bool newly = fault_mgr_set_external(&s_fm, id, active);
    xSemaphoreGive(s_fm_mutex);
    return newly;
}

bool shared_fault_try_reset(void)
{
    xSemaphoreTake(s_fm_mutex, portMAX_DELAY);
    const bool all_clear = fault_mgr_try_reset(&s_fm);
    xSemaphoreGive(s_fm_mutex);
    return all_clear;
}

void shared_fault_get(uint8_t *latched, uint8_t *active)
{
    xSemaphoreTake(s_fm_mutex, portMAX_DELAY);
    if (latched != NULL) {
        *latched = s_fm.latched;
    }
    if (active != NULL) {
        *active = s_fm.active;
    }
    xSemaphoreGive(s_fm_mutex);
}

/* ---- Watchdog ------------------------------------------------------------ */

void bms_task_watch(bms_task_id_t id, uint32_t timeout_ms)
{
    const uint32_t now = bms_now_ms();
    taskENTER_CRITICAL();
    wdg_register(&s_wdg, (unsigned)id, timeout_ms, now);
    taskEXIT_CRITICAL();
}

void bms_task_checkin(bms_task_id_t id)
{
    const uint32_t now = bms_now_ms();
    taskENTER_CRITICAL();
    wdg_checkin(&s_wdg, (unsigned)id, now);
    taskEXIT_CRITICAL();
}

uint32_t shared_wdg_check(uint32_t now_ms, uint32_t ages_ms[BMS_TASK_COUNT])
{
    taskENTER_CRITICAL();
    const uint32_t stalled = wdg_check(&s_wdg, now_ms);
    for (unsigned i = 0; i < (unsigned)BMS_TASK_COUNT; i++) {
        ages_ms[i] = wdg_age_ms(&s_wdg, i, now_ms);
    }
    taskEXIT_CRITICAL();
    return stalled;
}

/* ---- Test hooks ---------------------------------------------------------- */

void bms_test_hook_request_stall(uint8_t task_id, uint16_t duration_ms)
{
#if BMS_ENABLE_TEST_HOOKS
    if (task_id < (uint8_t)BMS_TASK_COUNT) {
        taskENTER_CRITICAL();
        s_stall_ms[task_id] = duration_ms;
        taskEXIT_CRITICAL();
    }
#else
    (void)task_id;
    (void)duration_ms;
#endif
}

void bms_test_hook_poll(bms_task_id_t id)
{
#if BMS_ENABLE_TEST_HOOKS
    taskENTER_CRITICAL();
    const uint16_t ms = s_stall_ms[id];
    s_stall_ms[id] = 0u;
    taskEXIT_CRITICAL();

    if (ms > 0u) {
        /* Simulates a task stuck on a deadlock or a hung driver call: it is
         * blocked and stops checking in, while the rest of the system runs. */
        bms_log("TEST", "stalling task '%s' for %u ms", bms_task_name(id), (unsigned)ms);
        vTaskDelay(pdMS_TO_TICKS(ms));
        bms_log("TEST", "task '%s' resumed", bms_task_name(id));
    }
#else
    (void)id;
#endif
}
