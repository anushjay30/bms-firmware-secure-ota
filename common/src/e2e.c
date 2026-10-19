#include "e2e.h"

#include "crc8.h"

#define E2E_MIN_DLC 2u

uint8_t e2e_compute_crc(const can_frame_t *frame)
{
    const uint8_t data_id[2] = {
        (uint8_t)(frame->id & 0xFFu),
        (uint8_t)((frame->id >> 8) & 0xFFu),
    };
    const uint8_t dlc = (frame->dlc > CAN_MAX_DLC) ? (uint8_t)CAN_MAX_DLC : frame->dlc;
    const size_t payload_len = (dlc > 1u) ? (size_t)dlc - 1u : 0u;

    uint8_t crc = crc8_sae_j1850_update(CRC8_SAE_J1850_INIT, data_id, sizeof data_id);
    crc = crc8_sae_j1850_update(crc, &frame->data[1], payload_len);
    return (uint8_t)(crc ^ CRC8_SAE_J1850_XOROUT);
}

void e2e_protect(can_frame_t *frame, uint8_t *counter)
{
    frame->data[1] = (uint8_t)((frame->data[1] & 0xF0u) | (*counter & 0x0Fu));
    frame->data[0] = e2e_compute_crc(frame);
    *counter = (uint8_t)((*counter + 1u) & E2E_COUNTER_MAX);
}

e2e_result_t e2e_check(e2e_rx_state_t *state, const can_frame_t *frame)
{
    if (frame->dlc < E2E_MIN_DLC) {
        return E2E_ERR_LENGTH;
    }
    if (frame->data[0] != e2e_compute_crc(frame)) {
        return E2E_ERR_CRC;
    }

    const uint8_t counter = (uint8_t)(frame->data[1] & 0x0Fu);
    /* Commands are sporadic, so we only reject an *unchanged* counter (a
     * replayed/frozen frame). A cyclic message would also bound the jump
     * (AUTOSAR "MaxDeltaCounter") to detect lost frames. */
    if (state->synced && counter == state->last_counter) {
        return E2E_ERR_REPEATED;
    }
    state->last_counter = counter;
    state->synced = true;
    return E2E_OK;
}

const char *e2e_result_name(e2e_result_t r)
{
    switch (r) {
    case E2E_OK:           return "OK";
    case E2E_ERR_LENGTH:   return "bad length";
    case E2E_ERR_CRC:      return "CRC mismatch";
    case E2E_ERR_REPEATED: return "repeated counter";
    default:               return "?";
    }
}
