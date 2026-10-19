/*
 * slcan.h - slcan / LAWICEL "serial line CAN" ASCII protocol (pure C).
 *
 * The de-facto protocol of cheap USB-CAN adapters (CANable, CANtact, USBtin)
 * and of the Linux slcan driver. One ASCII line per frame or command,
 * terminated by CR:
 *
 *   t1238DEADBEEF00112233\r   standard frame: 't', 3 hex ID, DLC digit, 2 hex per byte
 *   T123456788...\r           extended frame (8 hex ID)   - not used by the BMS
 *   r1230\r / R...\r          remote (RTR) frames         - not used by the BMS
 *   O\r C\r S6\r V\r ...      commands: open, close, set bitrate, version, ...
 *
 * The device answers commands with CR (OK) or BEL 0x07 (error).
 *
 * This module is the protocol only - no UART, no RTOS - so it is unit tested
 * on the host (tests/test_slcan.c). The Cortex-M3 port feeds it bytes from
 * the UART1 interrupt stream; see port/qemu-mps2/can_slcan.c.
 */
#ifndef SLCAN_H
#define SLCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can_if.h"

/* Longest line we accept, without the CR: 'T' + 8 ID + DLC + 16 data
 * + 4 optional timestamp digits = 30. Anything longer is an overrun. */
#define SLCAN_MAX_LINE 30u
/* Longest frame we emit: 't' + 3 + 1 + 16 + CR. */
#define SLCAN_MAX_FRAME_TEXT 22u

typedef enum {
    SLCAN_EV_FRAME,    /* a valid standard data frame, in .frame              */
    SLCAN_EV_COMMAND,  /* a command line: .cmd plus .arg/.arg_len              */
    SLCAN_EV_ERROR     /* a line that had to be discarded, reason in .error    */
} slcan_event_type_t;

typedef enum {
    SLCAN_ERR_NONE = 0,
    SLCAN_ERR_OVERFLOW,    /* line longer than SLCAN_MAX_LINE (garbage or lost CR) */
    SLCAN_ERR_SYNTAX,      /* bad hex, wrong length, DLC > 8, ID > 0x7FF, ...      */
    SLCAN_ERR_UNSUPPORTED  /* well-formed but not for us: extended or RTR frame    */
} slcan_error_t;

typedef struct {
    slcan_event_type_t type;
    slcan_error_t      error;
    can_frame_t        frame;
    char               cmd;                 /* first character of a command line */
    char               arg[SLCAN_MAX_LINE]; /* rest of the line (not terminated)  */
    uint8_t            arg_len;
} slcan_event_t;

/* Incremental line parser. Bytes may arrive in any chunking - a frame split
 * across several UART reads, or several frames in one read. */
typedef struct {
    char    line[SLCAN_MAX_LINE];
    uint8_t len;
    bool    overflow; /* discarding until the next line end */
} slcan_parser_t;

void slcan_parser_init(slcan_parser_t *p);

/* Feed one byte. Returns true when a line ended and *ev was filled in. Empty
 * lines (e.g. the LF of a CRLF pair) produce no event. */
bool slcan_parser_feed(slcan_parser_t *p, uint8_t byte, slcan_event_t *ev);

/* Decode one complete line (without its terminator). Exposed for tests. */
void slcan_decode_line(const char *line, size_t len, slcan_event_t *ev);

/* Encode a standard frame as "tIIIL<data>\r". Returns the length written
 * (including the CR), or 0 if the frame is invalid or cap is too small.
 * No NUL terminator is written. */
size_t slcan_encode_frame(const can_frame_t *frame, char *buf, size_t cap);

/* ---- Device side of the command set ------------------------------------- */

typedef struct {
    bool open;      /* channel open: frames are exchanged only while open */
    char bitrate;   /* last 'S' argument, '0'..'8' (S6 = 500 kbit/s)       */
} slcan_device_t;

/* Apply a command event to the device state and build the reply bytes
 * (CR = OK, BEL = error, or data + CR for queries). Returns the reply length. */
size_t slcan_device_command(slcan_device_t *dev, const slcan_event_t *ev, char *reply, size_t cap);

#endif /* SLCAN_H */
