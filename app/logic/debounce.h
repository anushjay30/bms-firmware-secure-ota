/*
 * debounce.h - Counter-based debounce filter.
 *
 * A raw condition only changes the filtered output after it has been
 * observed for N *consecutive* samples; any sample that disagrees restarts
 * the count. Separate set/clear counts let a fault trip quickly but require
 * a longer run of good samples before it is considered gone.
 */
#ifndef DEBOUNCE_H
#define DEBOUNCE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t set_count;    /* consecutive true samples to become active   */
    uint8_t clear_count;  /* consecutive false samples to become inactive */
    uint8_t counter;      /* length of the current disagreeing run        */
    bool    active;       /* filtered output                              */
} debounce_t;

/* Counts of 0 are treated as 1 (react on the first sample). */
void debounce_init(debounce_t *d, uint8_t set_count, uint8_t clear_count);

/* Feed one raw sample, returns the filtered state. */
bool debounce_update(debounce_t *d, bool raw);

/* Raw "value above limit" test with hysteresis: once active, the value must
 * drop below (limit - hyst) before it counts as good again. This stops a
 * value hovering at the limit from toggling the fault every sample. */
bool debounce_above(int32_t value, int32_t limit, int32_t hyst, bool active);

/* Mirror image for "value below limit" (undervoltage). */
bool debounce_below(int32_t value, int32_t limit, int32_t hyst, bool active);

#endif /* DEBOUNCE_H */
