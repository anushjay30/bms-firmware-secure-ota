/*
 * bms_hal.h - Minimal hardware abstraction implemented by each port.
 *
 * The application decides *what* should happen; the port decides *how*
 * (a log line or exit() on POSIX, register writes on the Cortex-M3 board).
 */
#ifndef BMS_HAL_H
#define BMS_HAL_H

#include <stdbool.h>
#include <stddef.h>

/* Drive the three HV relays: negative main, precharge, positive main. */
void hal_contactors_set(bool neg, bool pre, bool pos);

/* Write raw text to the console (stdout on POSIX, a UART on hardware).
 * Only the logger task calls this once the scheduler is running. */
void hal_console_write(const char *buf, size_t len);

/* Restart the whole ECU (Cortex-M: AIRCR.SYSRESETREQ). Does not return.
 * Used by the test harness to get a fresh firmware between scenarios, and
 * the natural last step of any unrecoverable error handler. */
void hal_system_reset(void);

/* Test hook only: execute an illegal operation so the fault handler can be
 * demonstrated. Does not return on ports that have a fault handler. */
void hal_trigger_fault(void);

#endif /* BMS_HAL_H */
