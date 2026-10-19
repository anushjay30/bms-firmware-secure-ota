/*
 * uart_stream.h - Interrupt-driven byte stream on UART1 (the slcan CAN link).
 *
 * RX and TX interrupts move bytes between the UART and FreeRTOS stream
 * buffers; tasks read and write the buffers.
 */
#ifndef UART_STREAM_H
#define UART_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_dropped;       /* RX stream buffer full: byte lost       */
    uint32_t rx_hw_overruns;   /* byte arrived before the ISR read the last one */
    uint32_t tx_bytes;
} uart_stream_stats_t;

/* Create the stream buffers and enable the UART1 interrupts. Call before the
 * scheduler starts (interrupts are held off until it does). */
void uart1_stream_init(uint32_t baud);

/* Wait up to timeout_ticks for at least one byte, then return as many as are
 * available (up to len). Returns 0 on timeout. Single reader only. */
size_t uart1_stream_read(uint8_t *buf, size_t len, uint32_t timeout_ticks);

/* Queue len bytes for transmission, all or nothing; never waits for space.
 * Safe to call from several tasks. Returns false if there was no room. */
bool uart1_stream_write(const uint8_t *buf, size_t len);

void uart1_stream_get_stats(uart_stream_stats_t *stats);

#endif /* UART_STREAM_H */
