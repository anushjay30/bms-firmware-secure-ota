/*
 * can_slcan.c - can_if implementation: slcan (LAWICEL) over UART1.
 *
 *   receive:  UART1 RX IRQ -> stream buffer -> can_if_receive(): slcan parser -> frame
 *   transmit: can_if_send(): slcan encoder -> stream buffer -> UART1 TX IRQ -> wire
 *
 * Layering: uart_stream.c moves bytes, common/slcan.c turns bytes into frames
 * and back, this file glues them to the can_if interface the application
 * uses. On the host, slcand hands the other end of the serial line to the
 * Linux slcan driver, which exposes it as the SocketCAN interface slcan0 -
 * so the Python simulator and test harness are unchanged.
 */
#include "can_if.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bms_port_config.h"
#include "slcan.h"
#include "uart_stream.h"

#define CAN_MAX_FILTERS 16u
#define RX_CHUNK        64u

static uint32_t s_accept[CAN_MAX_FILTERS];
static size_t s_num_accept;

/* Parser state persists across can_if_receive() calls: a line may arrive in
 * several pieces, and one UART read may contain the end of one frame plus
 * the start of the next. Bytes read but not yet parsed wait in s_chunk. */
static slcan_parser_t s_parser;
static slcan_device_t s_dev;
static uint8_t s_chunk[RX_CHUNK];
static size_t s_chunk_len;
static size_t s_chunk_pos;

/* Single writer each (RX counters: the CAN RX task; TX counters: the CAN TX
 * task), so plain aligned 32-bit variables are safe to read anywhere. */
static volatile uint32_t s_rx_frames;
static volatile uint32_t s_rx_errors;
static volatile uint32_t s_tx_frames;
static volatile uint32_t s_tx_errors;

can_if_status_t can_if_init(const char *channel, const uint32_t *accept_ids, size_t num_ids)
{
    (void)channel; /* one CAN link on this board: UART1 */
    if (num_ids > CAN_MAX_FILTERS || (num_ids > 0u && accept_ids == NULL)) {
        return CAN_IF_ERROR;
    }
    memcpy(s_accept, accept_ids, num_ids * sizeof s_accept[0]);
    s_num_accept = num_ids;

    slcan_parser_init(&s_parser);
    /* A USB-CAN adapter powers up closed and waits for the host's 'O'. This
     * end is the ECU itself, which must be on the bus from reset - after a
     * watchdog reset nobody would send 'O' again - so it starts open. 'C'
     * still closes it for protocol completeness. */
    s_dev.open = true;
    s_dev.bitrate = '6';

    uart1_stream_init(BMS_UART_BAUD);
    return CAN_IF_OK;
}

/* Software acceptance filter: what a CAN controller's filter banks would do
 * in hardware, done per frame here because slcan has no ID-list filter. */
static bool accepted(uint32_t id)
{
    if (s_num_accept == 0u) {
        return true;
    }
    for (size_t i = 0; i < s_num_accept; i++) {
        if (s_accept[i] == id) {
            return true;
        }
    }
    return false;
}

/* Handle one parsed line. Returns true if it produced a frame for the app. */
static bool handle_event(const slcan_event_t *ev, can_frame_t *frame)
{
    if (ev->type == SLCAN_EV_FRAME) {
        if (s_dev.open && accepted(ev->frame.id)) {
            *frame = ev->frame;
            s_rx_frames++;
            return true;
        }
        return false; /* closed or filtered out: drop silently */
    }
    if (ev->type == SLCAN_EV_ERROR) {
        s_rx_errors++; /* overlong line, bad hex, extended/RTR frame, ... */
    }
    /* Commands (and rejected lines) get a protocol reply: CR or BEL. */
    char reply[16];
    const size_t n = slcan_device_command(&s_dev, ev, reply, sizeof reply);
    if (n > 0u) {
        (void)uart1_stream_write((const uint8_t *)reply, n);
    }
    return false;
}

can_if_status_t can_if_receive(can_frame_t *frame, uint32_t timeout_ms)
{
    if (frame == NULL) {
        return CAN_IF_ERROR;
    }

    /* One logical timeout across several blocking reads (a frame may need
     * more than one): vTaskSetTimeOutState/xTaskCheckForTimeOut track the
     * remaining time correctly, including across tick counter wrap. */
    TimeOut_t timeout;
    TickType_t ticks_left = pdMS_TO_TICKS(timeout_ms);
    vTaskSetTimeOutState(&timeout);

    for (;;) {
        while (s_chunk_pos < s_chunk_len) {
            slcan_event_t ev;
            if (slcan_parser_feed(&s_parser, s_chunk[s_chunk_pos++], &ev) && handle_event(&ev, frame)) {
                return CAN_IF_OK; /* unparsed bytes stay in s_chunk for next time */
            }
        }
        if (xTaskCheckForTimeOut(&timeout, &ticks_left) != pdFALSE) {
            return CAN_IF_TIMEOUT;
        }
        s_chunk_pos = 0u;
        s_chunk_len = uart1_stream_read(s_chunk, sizeof s_chunk, ticks_left);
        if (s_chunk_len == 0u) {
            return CAN_IF_TIMEOUT;
        }
    }
}

can_if_status_t can_if_send(const can_frame_t *frame)
{
    char line[SLCAN_MAX_FRAME_TEXT];
    const size_t n = (frame != NULL) ? slcan_encode_frame(frame, line, sizeof line) : 0u;

    if (n == 0u || !s_dev.open || !uart1_stream_write((const uint8_t *)line, n)) {
        s_tx_errors++; /* invalid frame, channel closed by host, or TX buffer full */
        return CAN_IF_ERROR;
    }
    s_tx_frames++;
    return CAN_IF_OK;
}

void can_if_get_stats(can_if_stats_t *stats)
{
    uart_stream_stats_t u;
    uart1_stream_get_stats(&u);
    stats->rx_frames = s_rx_frames;
    stats->rx_overruns = u.rx_dropped + u.rx_hw_overruns;
    stats->rx_errors = s_rx_errors;
    stats->tx_frames = s_tx_frames;
    stats->tx_errors = s_tx_errors;
}
