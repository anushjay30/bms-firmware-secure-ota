/*
 * can_fifo.h - Lock-free single-producer / single-consumer FIFO of CAN frames.
 *
 * WHY lock-free: the producer and consumer run in contexts that must never
 * block on each other. On the POSIX port the producer is a plain pthread doing
 * blocking socket reads and the consumer is the FreeRTOS tick "interrupt"
 * (a signal handler) - taking a mutex inside a signal handler can deadlock.
 * The same pattern works for any ISR -> task hand-off on a microcontroller.
 * With exactly one writer per index, C11 acquire/release atomics are enough:
 * no locks and no disabling interrupts.
 *
 * This plays the role of a CAN controller's hardware RX FIFO: if it fills up,
 * new frames are dropped and counted as overruns.
 */
#ifndef CAN_FIFO_H
#define CAN_FIFO_H

#include <stdatomic.h>
#include <stdbool.h>

#include "can_if.h"

#define CAN_FIFO_CAPACITY 64u /* must be a power of two */

typedef struct {
    can_frame_t  buf[CAN_FIFO_CAPACITY];
    atomic_uint  head;   /* next slot to write; only the producer stores it */
    atomic_uint  tail;   /* next slot to read;  only the consumer stores it */
} can_fifo_t;

void can_fifo_init(can_fifo_t *f);

/* Producer: returns false (frame dropped) when full. */
bool can_fifo_push(can_fifo_t *f, const can_frame_t *frame);

/* Consumer: copy the oldest frame without removing it. False when empty. */
bool can_fifo_peek(can_fifo_t *f, can_frame_t *frame);

/* Consumer: discard the oldest frame (call after a successful peek). */
void can_fifo_pop(can_fifo_t *f);

/* Snapshot of the fill level (exact only when called by producer or consumer). */
unsigned can_fifo_count(can_fifo_t *f);

#endif /* CAN_FIFO_H */
