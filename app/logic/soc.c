#include "soc.h"

#define MS_PER_HOUR 3600000LL
#define PCT_X100_FULL 10000LL

void soc_init(soc_t *s, uint32_t capacity_mah, uint16_t initial_pct_x100)
{
    if (initial_pct_x100 > PCT_X100_FULL) {
        initial_pct_x100 = (uint16_t)PCT_X100_FULL;
    }
    s->capacity_mams = (int64_t)capacity_mah * MS_PER_HOUR;
    s->charge_mams = s->capacity_mams * (int64_t)initial_pct_x100 / PCT_X100_FULL;
}

void soc_update(soc_t *s, int32_t current_ma, uint32_t dt_ms)
{
    s->charge_mams -= (int64_t)current_ma * (int64_t)dt_ms;

    if (s->charge_mams < 0) {
        s->charge_mams = 0;
    } else if (s->charge_mams > s->capacity_mams) {
        s->charge_mams = s->capacity_mams;
    }
}

uint16_t soc_pct_x100(const soc_t *s)
{
    if (s->capacity_mams <= 0) {
        return 0u;
    }
    /* Round to nearest instead of truncating. */
    return (uint16_t)((s->charge_mams * PCT_X100_FULL + s->capacity_mams / 2) / s->capacity_mams);
}
