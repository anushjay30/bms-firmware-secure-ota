#include "bms_log.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "bms_hal.h"
#include "bms_tasks.h"

#define LOG_QUEUE_LEN  48u
#define LOG_TAG_LEN    8u
#define LOG_TEXT_LEN   120u

typedef struct {
    uint32_t t_ms;
    char     tag[LOG_TAG_LEN];
    char     text[LOG_TEXT_LEN];
} log_rec_t;

static StaticQueue_t s_logq_struct;
static uint8_t s_logq_storage[LOG_QUEUE_LEN * sizeof(log_rec_t)];
static QueueHandle_t s_logq;
/* Only tasks touch this, so a critical section is enough. (A C11
 * atomic_fetch_add would need LDREX/STREX - fine on Cortex-M3, but a
 * library call on Cortex-M0, which has no exclusive access instructions.) */
static uint32_t s_dropped;

void bms_log_init(void)
{
    s_logq = xQueueCreateStatic(LOG_QUEUE_LEN, sizeof(log_rec_t), s_logq_storage, &s_logq_struct);
    configASSERT(s_logq != NULL);
}

static void emit(const log_rec_t *r)
{
    char line[LOG_TAG_LEN + LOG_TEXT_LEN + 32u];
    const int n = snprintf(line, sizeof line, "[%5lu.%03lu] %-6s %s\n",
                           (unsigned long)(r->t_ms / 1000u), (unsigned long)(r->t_ms % 1000u),
                           r->tag, r->text);
    if (n > 0) {
        hal_console_write(line, ((size_t)n < sizeof line) ? (size_t)n : sizeof line - 1u);
    }
}

void bms_log(const char *tag, const char *fmt, ...)
{
    log_rec_t r;
    r.t_ms = bms_now_ms();
    strncpy(r.tag, tag, LOG_TAG_LEN - 1u);
    r.tag[LOG_TAG_LEN - 1u] = '\0';

    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(r.text, sizeof r.text, fmt, ap);
    va_end(ap);

    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED || s_logq == NULL) {
        emit(&r); /* single-threaded startup: safe to write directly */
        return;
    }
    if (xQueueSend(s_logq, &r, 0) != pdPASS) {
        taskENTER_CRITICAL();
        s_dropped++;
        taskEXIT_CRITICAL();
    }
}

void bms_log_flush(uint32_t timeout_ms)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED || s_logq == NULL) {
        return; /* nothing is queued before the scheduler runs */
    }
    /* The logger is the lowest-priority task: it only drains the queue while
     * we (and everyone else) block. Poll with a 1-tick sleep. */
    TickType_t waited = 0;
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while (uxQueueMessagesWaiting(s_logq) > 0u && waited < limit) {
        vTaskDelay(1);
        waited++;
    }
    /* An empty queue means the last record has been taken, not yet written
     * out: give the logger a moment to finish that line. */
    vTaskDelay(pdMS_TO_TICKS(10));
}

void bms_logger_task(void *arg)
{
    (void)arg;
    uint32_t reported_drops = 0u;

    for (;;) {
        bms_task_checkin(BMS_TASK_LOGGER);

        log_rec_t r;
        if (xQueueReceive(s_logq, &r, pdMS_TO_TICKS(BMS_MONITOR_PERIOD_MS)) == pdPASS) {
            emit(&r);
        }

        taskENTER_CRITICAL();
        const uint32_t drops = s_dropped;
        taskEXIT_CRITICAL();
        if (drops != reported_drops) {
            log_rec_t d = { .t_ms = bms_now_ms(), .tag = "LOG" };
            (void)snprintf(d.text, sizeof d.text, "%lu log line(s) dropped (queue full)",
                           (unsigned long)(drops - reported_drops));
            emit(&d);
            reported_drops = drops;
        }
    }
}
