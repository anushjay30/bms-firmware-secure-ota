/*
 * bms_log.h - Timestamped, deferred logging.
 *
 * WHY deferred: formatting is cheap but console output is slow (a UART at
 * 115200 baud needs ~9 ms for 100 characters). Tasks format into a record and
 * queue it without blocking; a lowest-priority logger task does the slow
 * write. Only that one task touches the console, which also avoids stdio
 * locking problems on the POSIX port. If the queue is full, lines are dropped
 * and counted rather than delaying a safety-relevant task.
 */
#ifndef BMS_LOG_H
#define BMS_LOG_H

#include <stdint.h>

#if defined(__GNUC__)
#define BMS_PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define BMS_PRINTF_FMT(a, b)
#endif

void bms_log_init(void);

/* Callable from any task (not from interrupts). Before the scheduler starts,
 * it writes straight to the console. */
void bms_log(const char *tag, const char *fmt, ...) BMS_PRINTF_FMT(2, 3);

/* Block the calling task until every queued line has been written (or
 * timeout_ms passes). Call before a deliberate reset, so the last diagnostic
 * lines are not lost with the RAM they are queued in. */
void bms_log_flush(uint32_t timeout_ms);

void bms_logger_task(void *arg);

#endif /* BMS_LOG_H */
