#include "debounce.h"

void debounce_init(debounce_t *d, uint8_t set_count, uint8_t clear_count)
{
    d->set_count = (set_count == 0u) ? 1u : set_count;
    d->clear_count = (clear_count == 0u) ? 1u : clear_count;
    d->counter = 0u;
    d->active = false;
}

bool debounce_update(debounce_t *d, bool raw)
{
    if (raw == d->active) {
        d->counter = 0u; /* agrees with output: any pending run is broken */
        return d->active;
    }

    d->counter++;
    const uint8_t needed = d->active ? d->clear_count : d->set_count;
    if (d->counter >= needed) {
        d->active = raw;
        d->counter = 0u;
    }
    return d->active;
}

bool debounce_above(int32_t value, int32_t limit, int32_t hyst, bool active)
{
    return active ? (value > (limit - hyst)) : (value > limit);
}

bool debounce_below(int32_t value, int32_t limit, int32_t hyst, bool active)
{
    return active ? (value < (limit + hyst)) : (value < limit);
}
