/*
 * health.h - "Has the firmware been healthy for long enough?" (pure logic).
 *
 * A newly installed image runs on trial until it confirms itself. It may do
 * so only after it has proven itself: every task checking in with the
 * watchdog supervisor and no fault latched or active, continuously, for a
 * configurable period. Any unhealthy observation restarts the clock.
 */
#ifndef HEALTH_H
#define HEALTH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     healthy;  /* currently in a healthy stretch */
    uint32_t since_ms; /* when it started                */
} health_t;

void health_init(health_t *h);

/* Feed one observation. Returns true when the system has been healthy for
 * required_ms without interruption; the clock then restarts, so a caller
 * whose action failed is asked again one period later. */
bool health_update(health_t *h, uint32_t now_ms, bool tasks_alive, bool fault_free, uint32_t required_ms);

/* Length of the current healthy stretch (0 if unhealthy). */
uint32_t health_healthy_ms(const health_t *h, uint32_t now_ms);

#endif /* HEALTH_H */
