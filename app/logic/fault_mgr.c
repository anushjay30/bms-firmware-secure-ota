#include "fault_mgr.h"

#include "bms_config.h"

void fault_mgr_init(fault_mgr_t *fm)
{
    for (unsigned i = 0; i < BMS_NUM_CELLS; i++) {
        debounce_init(&fm->ov[i], BMS_DEBOUNCE_OV_SET, BMS_DEBOUNCE_CLEAR);
        debounce_init(&fm->uv[i], BMS_DEBOUNCE_UV_SET, BMS_DEBOUNCE_CLEAR);
        debounce_init(&fm->ot[i], BMS_DEBOUNCE_OT_SET, BMS_DEBOUNCE_CLEAR);
    }
    debounce_init(&fm->oc, BMS_DEBOUNCE_OC_SET, BMS_DEBOUNCE_CLEAR);
    debounce_init(&fm->meas_timeout, BMS_DEBOUNCE_MEAS_TIMEOUT_SET, 1u);
    fm->external_active = 0u;
    fm->active = 0u;
    fm->latched = 0u;
}

/* Run one debouncer per cell. Returns true if any cell is active and reports
 * the first cell that *became* active on this sample (1-based, 0 if none). */
typedef bool (*limit_fn)(int32_t value, int32_t limit, int32_t hyst, bool active);

static bool update_cells(debounce_t deb[], const int32_t values[], int32_t limit, int32_t hyst,
                         limit_fn check, uint8_t *new_cell, int32_t *new_value)
{
    bool any = false;
    *new_cell = 0u;
    for (unsigned i = 0; i < BMS_NUM_CELLS; i++) {
        const bool was = deb[i].active;
        const bool now = debounce_update(&deb[i], check(values[i], limit, hyst, was));
        if (now && !was && *new_cell == 0u) {
            *new_cell = (uint8_t)(i + 1u);
            *new_value = values[i];
        }
        any = any || now;
    }
    return any;
}

static void set_bit(uint8_t *field, bms_fault_id_t id, bool on)
{
    if (on) {
        *field = (uint8_t)(*field | BMS_FAULT_BIT(id));
    } else {
        *field = (uint8_t)(*field & (uint8_t)~BMS_FAULT_BIT(id));
    }
}

/* Recompute fm->active from a new measurement bitfield, latch rising bits and
 * record events. */
static size_t latch_new(fault_mgr_t *fm, uint8_t new_active, const bms_fault_event_t cand[],
                        bms_fault_event_t *events, size_t max_events)
{
    size_t n = 0;
    const uint8_t rising = (uint8_t)(new_active & (uint8_t)~fm->latched);
    for (unsigned id = 0; id < (unsigned)BMS_FAULT_COUNT; id++) {
        if ((rising & BMS_FAULT_BIT(id)) != 0u && n < max_events && events != NULL) {
            events[n++] = cand[id];
        }
    }
    fm->active = new_active;
    fm->latched = (uint8_t)(fm->latched | new_active);
    return n;
}

size_t fault_mgr_update(fault_mgr_t *fm, const bms_sample_t *s,
                        bms_fault_event_t *events, size_t max_events)
{
    bms_fault_event_t cand[BMS_FAULT_COUNT];
    for (unsigned id = 0; id < (unsigned)BMS_FAULT_COUNT; id++) {
        cand[id] = (bms_fault_event_t){ .id = (bms_fault_id_t)id, .cell = 0u, .value = 0 };
    }

    uint8_t act = fm->active;

    if (s->valid) {
        int32_t mv[BMS_NUM_CELLS];
        int32_t dc[BMS_NUM_CELLS];
        for (unsigned i = 0; i < BMS_NUM_CELLS; i++) {
            mv[i] = (int32_t)s->cell_mv[i];
            dc[i] = (int32_t)s->cell_dc[i];
        }

        bms_fault_event_t *e = &cand[BMS_FAULT_CELL_OV];
        set_bit(&act, BMS_FAULT_CELL_OV,
                update_cells(fm->ov, mv, BMS_CELL_OV_MV, BMS_CELL_OV_HYST_MV, debounce_above, &e->cell, &e->value));
        e = &cand[BMS_FAULT_CELL_UV];
        set_bit(&act, BMS_FAULT_CELL_UV,
                update_cells(fm->uv, mv, BMS_CELL_UV_MV, BMS_CELL_UV_HYST_MV, debounce_below, &e->cell, &e->value));
        e = &cand[BMS_FAULT_CELL_OT];
        set_bit(&act, BMS_FAULT_CELL_OT,
                update_cells(fm->ot, dc, BMS_CELL_OT_DC, BMS_CELL_OT_HYST_DC, debounce_above, &e->cell, &e->value));

        /* Overcurrent has different limits per direction, so build the raw
         * condition by hand; the hysteresis applies to both. */
        const int32_t ma = s->pack_ma;
        const bool oc_raw = debounce_above(ma, BMS_PACK_OC_DISCHARGE_MA, BMS_PACK_OC_HYST_MA, fm->oc.active) ||
                            debounce_above(-ma, BMS_PACK_OC_CHARGE_MA, BMS_PACK_OC_HYST_MA, fm->oc.active);
        set_bit(&act, BMS_FAULT_PACK_OC, debounce_update(&fm->oc, oc_raw));
        cand[BMS_FAULT_PACK_OC].value = ma;
    }

    set_bit(&act, BMS_FAULT_MEAS_TIMEOUT, debounce_update(&fm->meas_timeout, s->stale));
    cand[BMS_FAULT_MEAS_TIMEOUT].value = (int32_t)BMS_MEAS_TIMEOUT_MS;

    /* Keep externally owned conditions (watchdog) in the active set. */
    act = (uint8_t)((act & (uint8_t)~BMS_FAULT_BIT(BMS_FAULT_WATCHDOG)) | fm->external_active);

    return latch_new(fm, act, cand, events, max_events);
}

bool fault_mgr_latch(fault_mgr_t *fm, bms_fault_id_t id)
{
    const bool was = (fm->latched & BMS_FAULT_BIT(id)) != 0u;
    set_bit(&fm->latched, id, true);
    return !was;
}

bool fault_mgr_set_external(fault_mgr_t *fm, bms_fault_id_t id, bool active)
{
    set_bit(&fm->external_active, id, active);
    set_bit(&fm->active, id, active);
    return active ? fault_mgr_latch(fm, id) : false;
}

bool fault_mgr_try_reset(fault_mgr_t *fm)
{
    fm->latched = (uint8_t)(fm->latched & fm->active);
    return fm->latched == 0u;
}

const char *fault_name(bms_fault_id_t id)
{
    switch (id) {
    case BMS_FAULT_CELL_OV:      return "CELL_OV";
    case BMS_FAULT_CELL_UV:      return "CELL_UV";
    case BMS_FAULT_CELL_OT:      return "CELL_OT";
    case BMS_FAULT_PACK_OC:      return "PACK_OC";
    case BMS_FAULT_MEAS_TIMEOUT: return "MEAS_TIMEOUT";
    case BMS_FAULT_PRECHARGE:    return "PRECHARGE";
    case BMS_FAULT_WATCHDOG:     return "WATCHDOG";
    default:                     return "?";
    }
}
