/*
 * soc.h - State of charge by coulomb counting (pure logic).
 *
 * SoC(t) = SoC(0) - (1/capacity) * integral(I dt)
 *
 * Charge is accumulated in mA*ms in a 64-bit integer, so even a 10 mA
 * current over a 100 ms step (1000 mA*ms) is counted exactly - no rounding
 * error builds up over hours of driving, and no floating point is needed.
 *
 * Known limitation (interview talking point): pure coulomb counting drifts
 * with current-sensor offset error and does not know the true starting SoC.
 * Production BMSs correct it with open-circuit-voltage lookups at rest and/or
 * a Kalman filter on an equivalent-circuit cell model.
 */
#ifndef SOC_H
#define SOC_H

#include <stdint.h>

typedef struct {
    int64_t capacity_mams; /* full capacity, mA*ms */
    int64_t charge_mams;   /* remaining charge, mA*ms, clamped to [0, capacity] */
} soc_t;

void soc_init(soc_t *s, uint32_t capacity_mah, uint16_t initial_pct_x100);

/* Integrate current over dt. current_ma > 0 = discharge. */
void soc_update(soc_t *s, int32_t current_ma, uint32_t dt_ms);

/* SoC in hundredths of a percent, 0..10000. */
uint16_t soc_pct_x100(const soc_t *s);

#endif /* SOC_H */
