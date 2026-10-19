/*
 * watchdog.h - Software task watchdog (pure logic).
 *
 * WHY: a hardware watchdog only proves that *something* is still kicking it.
 * If one task deadlocks while the others run, the hardware watchdog stays
 * happy. A supervisor that requires every task to check in within its own
 * deadline catches the stalled task and lets us fail safe (open contactors).
 * On real hardware the supervisor would only kick the hardware watchdog while
 * all tasks are healthy, so a stuck supervisor still ends in a reset.
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#define WDG_MAX_TASKS 8u

typedef struct {
    uint32_t last_ms;
    uint32_t timeout_ms;
    bool     enabled;
} wdg_slot_t;

typedef struct {
    wdg_slot_t slot[WDG_MAX_TASKS];
} wdg_t;

void wdg_init(wdg_t *w);
void wdg_register(wdg_t *w, unsigned id, uint32_t timeout_ms, uint32_t now_ms);
void wdg_checkin(wdg_t *w, unsigned id, uint32_t now_ms);

/* Bitmask of registered tasks whose last check-in is older than their timeout. */
uint32_t wdg_check(const wdg_t *w, uint32_t now_ms);

/* Milliseconds since a task last checked in. */
uint32_t wdg_age_ms(const wdg_t *w, unsigned id, uint32_t now_ms);

#endif /* WATCHDOG_H */
