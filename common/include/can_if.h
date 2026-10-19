/*
 * can_if.h - Port-independent CAN driver interface.
 *
 * The application only ever talks to CAN through these functions. Each port
 * provides one implementation:
 *   - port/posix     : Linux SocketCAN (vcan0) with an RX pthread bridge
 *   - port/qemu-mps2 : slcan (LAWICEL) ASCII protocol over a CMSDK UART; on
 *                      the host, slcand turns that UART into SocketCAN slcan0
 * so moving to new hardware means writing one driver file, not touching app/.
 */
#ifndef CAN_IF_H
#define CAN_IF_H

#include <stddef.h>
#include <stdint.h>

#define CAN_MAX_DLC 8u

/* Classic CAN 2.0A frame with an 11-bit standard identifier. */
typedef struct {
    uint32_t id;               /* 0x000..0x7FF                       */
    uint8_t  dlc;              /* payload length 0..8                */
    uint8_t  data[CAN_MAX_DLC];
} can_frame_t;

typedef enum {
    CAN_IF_OK = 0,
    CAN_IF_TIMEOUT,            /* nothing received within the timeout     */
    CAN_IF_ERROR               /* driver/bus error or invalid argument    */
} can_if_status_t;

typedef struct {
    uint32_t rx_frames;        /* frames delivered to the application     */
    uint32_t rx_overruns;      /* input dropped because a receive buffer was full */
    uint32_t rx_errors;        /* malformed or unsupported input discarded */
    uint32_t tx_frames;
    uint32_t tx_errors;
} can_if_stats_t;

/*
 * Open the CAN channel and install an acceptance filter so only the listed
 * identifiers reach the application (like the hardware filter banks on a real
 * CAN controller, this keeps bus traffic we do not care about out of the CPU).
 * Must be called before the scheduler starts.
 */
can_if_status_t can_if_init(const char *channel, const uint32_t *accept_ids, size_t num_ids);

/* Queue one frame for transmission. Never waits for bus or buffer space:
 * if the transmit path is full the frame is rejected (like a busy mailbox). */
can_if_status_t can_if_send(const can_frame_t *frame);

/* Block the calling task until a frame arrives or timeout_ms elapses. */
can_if_status_t can_if_receive(can_frame_t *frame, uint32_t timeout_ms);

void can_if_get_stats(can_if_stats_t *stats);

#endif /* CAN_IF_H */
