#include "meas.h"

#include <string.h>

#define SEEN_VOLT(page) ((uint16_t)(1u << (page)))
#define SEEN_TEMP(page) ((uint16_t)(1u << (MEAS_PAGES + (page))))
#define SEEN_PACK       ((uint16_t)(1u << (2u * MEAS_PAGES)))
#define SEEN_ALL        ((uint16_t)((1u << (2u * MEAS_PAGES + 1u)) - 1u))

void meas_store_init(meas_store_t *m)
{
    memset(m, 0, sizeof *m);
}

bool meas_store_cell_voltages(meas_store_t *m, uint8_t page, const uint16_t mv[MEAS_CELLS_PER_PAGE], uint32_t now_ms)
{
    if (page >= MEAS_PAGES) {
        return false;
    }
    for (unsigned i = 0; i < MEAS_CELLS_PER_PAGE; i++) {
        m->cell_mv[page * MEAS_CELLS_PER_PAGE + i] = mv[i];
    }
    m->volt_rx_ms[page] = now_ms;
    m->seen = (uint16_t)(m->seen | SEEN_VOLT(page));
    return true;
}

bool meas_store_cell_temps(meas_store_t *m, uint8_t page, const int16_t dc[MEAS_CELLS_PER_PAGE], uint32_t now_ms)
{
    if (page >= MEAS_PAGES) {
        return false;
    }
    for (unsigned i = 0; i < MEAS_CELLS_PER_PAGE; i++) {
        m->cell_dc[page * MEAS_CELLS_PER_PAGE + i] = dc[i];
    }
    m->temp_rx_ms[page] = now_ms;
    m->seen = (uint16_t)(m->seen | SEEN_TEMP(page));
    return true;
}

void meas_store_pack(meas_store_t *m, int32_t pack_ma, uint16_t link_mv, uint32_t now_ms)
{
    m->pack_ma = pack_ma;
    m->link_mv = link_mv;
    m->pack_rx_ms = now_ms;
    m->seen = (uint16_t)(m->seen | SEEN_PACK);
}

static bool fresh(uint32_t rx_ms, uint32_t now_ms, uint32_t timeout_ms)
{
    return (uint32_t)(now_ms - rx_ms) <= timeout_ms;
}

void meas_build_sample(const meas_store_t *m, uint32_t now_ms, uint32_t timeout_ms,
                       bool *ever_valid, bms_sample_t *out)
{
    bool valid = (m->seen == SEEN_ALL) && fresh(m->pack_rx_ms, now_ms, timeout_ms);
    for (unsigned p = 0; p < MEAS_PAGES; p++) {
        valid = valid && fresh(m->volt_rx_ms[p], now_ms, timeout_ms) && fresh(m->temp_rx_ms[p], now_ms, timeout_ms);
    }

    out->t_ms = now_ms;
    out->pack_mv = 0u;
    for (unsigned i = 0; i < BMS_NUM_CELLS; i++) {
        out->cell_mv[i] = m->cell_mv[i];
        out->cell_dc[i] = m->cell_dc[i];
        out->pack_mv += m->cell_mv[i];
    }
    out->pack_ma = m->pack_ma;
    out->link_mv = m->link_mv;
    out->valid = valid;
    out->stale = *ever_valid && !valid;
    if (valid) {
        *ever_valid = true;
    }
}
