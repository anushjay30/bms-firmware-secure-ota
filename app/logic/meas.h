/*
 * meas.h - Latest-value store for incoming measurements (pure logic).
 *
 * The CAN RX task writes each decoded frame into the store along with its
 * arrival time. The monitor task periodically turns the store into one
 * bms_sample_t, deciding whether the data is complete and fresh. Timestamps
 * per frame group let us detect a sensor that has gone silent even while
 * others keep talking.
 */
#ifndef MEAS_H
#define MEAS_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_types.h"

#define MEAS_CELLS_PER_PAGE 3u
#define MEAS_PAGES          (BMS_NUM_CELLS / MEAS_CELLS_PER_PAGE)

typedef struct {
    uint16_t cell_mv[BMS_NUM_CELLS];
    int16_t  cell_dc[BMS_NUM_CELLS];
    int32_t  pack_ma;
    uint16_t link_mv;
    uint32_t volt_rx_ms[MEAS_PAGES];
    uint32_t temp_rx_ms[MEAS_PAGES];
    uint32_t pack_rx_ms;
    uint16_t seen;      /* bit per group received at least once */
} meas_store_t;

void meas_store_init(meas_store_t *m);
bool meas_store_cell_voltages(meas_store_t *m, uint8_t page, const uint16_t mv[MEAS_CELLS_PER_PAGE], uint32_t now_ms);
bool meas_store_cell_temps(meas_store_t *m, uint8_t page, const int16_t dc[MEAS_CELLS_PER_PAGE], uint32_t now_ms);
void meas_store_pack(meas_store_t *m, int32_t pack_ma, uint16_t link_mv, uint32_t now_ms);

/*
 * Build a sample from the store at time now_ms.
 *   valid = every group has been received and none is older than timeout_ms
 *   stale = data was valid at some point (*ever_valid) but is not any more
 * Tracking "ever valid" means a BMS that boots before its sensors does not
 * immediately latch a timeout fault; it simply refuses to close contactors.
 */
void meas_build_sample(const meas_store_t *m, uint32_t now_ms, uint32_t timeout_ms,
                       bool *ever_valid, bms_sample_t *out);

#endif /* MEAS_H */
