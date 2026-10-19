/*
 * e2e.h - End-to-end (E2E) protection for safety-relevant CAN messages.
 *
 * WHY: CAN's own CRC-15 only protects a frame on the wire. It cannot detect a
 * gateway or software bug that corrupts data, a sender that is stuck repeating
 * an old frame, or a frame delivered under the wrong ID. AUTOSAR E2E adds,
 * inside the payload:
 *   - a CRC computed over a per-message Data ID + the payload, so corruption
 *     and wrong-ID delivery are both detected, and
 *   - a 4-bit alive counter that the sender increments on every transmission,
 *     so a frozen or replayed frame is detected.
 *
 * Layout used by every protected message in this project (similar in spirit
 * to AUTOSAR E2E Profile 1 with CRC at byte 0):
 *   byte 0       : CRC-8 SAE J1850 over [DataID lo, DataID hi, data[1..dlc-1]]
 *   byte 1 [3:0] : alive counter 0..15
 * The Data ID is the 11-bit CAN identifier.
 */
#ifndef E2E_H
#define E2E_H

#include <stdbool.h>
#include <stdint.h>

#include "can_if.h"

#define E2E_COUNTER_MAX 15u

typedef enum {
    E2E_OK = 0,
    E2E_ERR_LENGTH,     /* frame too short to carry CRC + counter         */
    E2E_ERR_CRC,        /* corrupted, or delivered under the wrong ID     */
    E2E_ERR_REPEATED    /* same counter as the previous frame: stale data */
} e2e_result_t;

typedef struct {
    uint8_t last_counter;
    bool    synced;     /* false until the first valid frame is seen */
} e2e_rx_state_t;

/* CRC over Data ID + payload bytes 1..dlc-1 (byte 0 holds the CRC itself). */
uint8_t e2e_compute_crc(const can_frame_t *frame);

/* Sender side: write *counter into byte 1, compute CRC into byte 0, then
 * advance *counter (wrapping 15 -> 0). frame->id/dlc/payload must be set. */
void e2e_protect(can_frame_t *frame, uint8_t *counter);

/* Receiver side: verify CRC and that the counter moved on. */
e2e_result_t e2e_check(e2e_rx_state_t *state, const can_frame_t *frame);

const char *e2e_result_name(e2e_result_t r);

#endif /* E2E_H */
