#include "bms_msgs.h"

#include <string.h>

/* Explicit byte-by-byte little-endian access: portable regardless of host
 * endianness or alignment (never cast a uint8_t* to a uint32_t*). */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void frame_init(can_frame_t *f, uint32_t id, uint8_t dlc)
{
    memset(f, 0, sizeof *f);
    f->id = id;
    f->dlc = dlc;
}

void bms_msg_pack_status(const bms_status_msg_t *m, uint8_t *counter, can_frame_t *f)
{
    frame_init(f, BMS_ID_STATUS, 8u);
    /* byte1: [3:0] alive counter (set by e2e_protect), [6:4] state, [7] meas valid */
    f->data[1] = (uint8_t)((((unsigned)m->state & 0x7u) << 4) | (m->meas_valid ? 0x80u : 0u));
    f->data[2] = m->faults_latched;
    f->data[3] = m->faults_active;
    put_u16(&f->data[4], m->soc_pct_x100);
    put_u16(&f->data[6], m->pack_mv);
    e2e_protect(f, counter);
}

void bms_msg_pack_fault_event(const bms_fault_event_t *e, uint8_t faults_latched,
                              uint8_t event_counter, can_frame_t *f)
{
    frame_init(f, BMS_ID_FAULT_EVENT, 8u);
    f->data[0] = (uint8_t)e->id;
    f->data[1] = e->cell;
    put_u32(&f->data[2], (uint32_t)e->value);
    f->data[6] = faults_latched;
    f->data[7] = event_counter;
}

void bms_msg_pack_command(bms_cmd_t cmd, uint8_t *counter, can_frame_t *f)
{
    frame_init(f, BMS_ID_VCU_COMMAND, 8u);
    f->data[2] = (uint8_t)cmd;
    e2e_protect(f, counter);
}

bool bms_msg_unpack_cell_voltages(const can_frame_t *f, uint8_t *page, uint16_t mv[MEAS_CELLS_PER_PAGE])
{
    if (f->id != BMS_ID_CELL_VOLTAGES || f->dlc < 7u || f->data[0] >= MEAS_PAGES) {
        return false;
    }
    *page = f->data[0];
    for (unsigned i = 0; i < MEAS_CELLS_PER_PAGE; i++) {
        mv[i] = get_u16(&f->data[1u + 2u * i]);
    }
    return true;
}

bool bms_msg_unpack_cell_temps(const can_frame_t *f, uint8_t *page, int16_t dc[MEAS_CELLS_PER_PAGE])
{
    if (f->id != BMS_ID_CELL_TEMPS || f->dlc < 7u || f->data[0] >= MEAS_PAGES) {
        return false;
    }
    *page = f->data[0];
    for (unsigned i = 0; i < MEAS_CELLS_PER_PAGE; i++) {
        dc[i] = (int16_t)get_u16(&f->data[1u + 2u * i]);
    }
    return true;
}

bool bms_msg_unpack_pack_meas(const can_frame_t *f, int32_t *pack_ma, uint16_t *link_mv)
{
    if (f->id != BMS_ID_PACK_MEAS || f->dlc < 6u) {
        return false;
    }
    *pack_ma = (int32_t)get_u32(&f->data[0]);
    *link_mv = get_u16(&f->data[4]);
    return true;
}

e2e_result_t bms_msg_unpack_command(const can_frame_t *f, e2e_rx_state_t *e2e, bms_cmd_t *cmd)
{
    if (f->id != BMS_ID_VCU_COMMAND || f->dlc < 3u) {
        return E2E_ERR_LENGTH;
    }
    const e2e_result_t r = e2e_check(e2e, f);
    if (r == E2E_OK) {
        const uint8_t raw = f->data[2];
        *cmd = (raw <= (uint8_t)BMS_CMD_RESET) ? (bms_cmd_t)raw : BMS_CMD_NONE;
    }
    return r;
}

bool bms_msg_unpack_test_hook(const can_frame_t *f, bms_test_hook_msg_t *m)
{
    if (f->id != BMS_ID_TEST_HOOK || f->dlc < 4u) {
        return false;
    }
    /* Only known hook IDs are passed through; anything else becomes NONE so a
     * corrupted or future debug frame can never trigger an unexpected action. */
    m->hook = (f->data[0] <= (uint8_t)BMS_HOOK_FLASH_POWER_LOSS) ? (bms_hook_id_t)f->data[0] : BMS_HOOK_NONE;
    m->task_id = f->data[1];
    m->duration_ms = get_u16(&f->data[2]);
    m->param = (f->dlc >= 8u) ? get_u32(&f->data[4]) : 0u; /* older 4-byte frames: no parameter */
    return true;
}
