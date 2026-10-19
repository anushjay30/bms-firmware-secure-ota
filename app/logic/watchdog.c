#include "watchdog.h"

void wdg_init(wdg_t *w)
{
    for (unsigned i = 0; i < WDG_MAX_TASKS; i++) {
        w->slot[i] = (wdg_slot_t){ .last_ms = 0u, .timeout_ms = 0u, .enabled = false };
    }
}

void wdg_register(wdg_t *w, unsigned id, uint32_t timeout_ms, uint32_t now_ms)
{
    if (id < WDG_MAX_TASKS) {
        w->slot[id] = (wdg_slot_t){ .last_ms = now_ms, .timeout_ms = timeout_ms, .enabled = true };
    }
}

void wdg_checkin(wdg_t *w, unsigned id, uint32_t now_ms)
{
    if (id < WDG_MAX_TASKS) {
        w->slot[id].last_ms = now_ms;
    }
}

uint32_t wdg_age_ms(const wdg_t *w, unsigned id, uint32_t now_ms)
{
    return (id < WDG_MAX_TASKS) ? (uint32_t)(now_ms - w->slot[id].last_ms) : 0u;
}

uint32_t wdg_check(const wdg_t *w, uint32_t now_ms)
{
    uint32_t stalled = 0u;
    for (unsigned i = 0; i < WDG_MAX_TASKS; i++) {
        if (w->slot[i].enabled && wdg_age_ms(w, i, now_ms) > w->slot[i].timeout_ms) {
            stalled |= (1u << i);
        }
    }
    return stalled;
}
