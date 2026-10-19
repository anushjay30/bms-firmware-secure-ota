#include "can_fifo.h"

#define CAN_FIFO_MASK (CAN_FIFO_CAPACITY - 1u)

_Static_assert((CAN_FIFO_CAPACITY & CAN_FIFO_MASK) == 0u, "CAN_FIFO_CAPACITY must be a power of two");
/* The FIFO is used between an interrupt/signal context and a task, so the
 * index accesses must never fall back to a lock inside libatomic (a lock taken
 * in an ISR could deadlock against the interrupted task). We only use plain
 * loads and stores - no read-modify-write - which are lock-free even on
 * ARMv6-M (Cortex-M0); this makes that assumption a compile-time check. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "atomic_uint must be lock-free for ISR use");

/*
 * head and tail are free-running counters; the slot index is counter & MASK.
 * Unsigned wrap-around keeps (head - tail) equal to the number of stored
 * frames even after the counters overflow.
 *
 * Memory ordering:
 *  - The producer writes the frame into buf[] and *then* publishes it with a
 *    release-store of head. The consumer's acquire-load of head guarantees it
 *    sees the completed frame, never a half-written one.
 *  - Symmetrically, the consumer releases a slot with a release-store of tail
 *    after it has finished copying it out, so the producer cannot overwrite
 *    a slot that is still being read.
 */

void can_fifo_init(can_fifo_t *f)
{
    atomic_init(&f->head, 0u);
    atomic_init(&f->tail, 0u);
}

bool can_fifo_push(can_fifo_t *f, const can_frame_t *frame)
{
    const unsigned head = atomic_load_explicit(&f->head, memory_order_relaxed);
    const unsigned tail = atomic_load_explicit(&f->tail, memory_order_acquire);

    if ((head - tail) >= CAN_FIFO_CAPACITY) {
        return false; /* full: overrun */
    }
    f->buf[head & CAN_FIFO_MASK] = *frame;
    atomic_store_explicit(&f->head, head + 1u, memory_order_release);
    return true;
}

bool can_fifo_peek(can_fifo_t *f, can_frame_t *frame)
{
    const unsigned tail = atomic_load_explicit(&f->tail, memory_order_relaxed);
    const unsigned head = atomic_load_explicit(&f->head, memory_order_acquire);

    if (head == tail) {
        return false; /* empty */
    }
    *frame = f->buf[tail & CAN_FIFO_MASK];
    return true;
}

void can_fifo_pop(can_fifo_t *f)
{
    const unsigned tail = atomic_load_explicit(&f->tail, memory_order_relaxed);
    atomic_store_explicit(&f->tail, tail + 1u, memory_order_release);
}

unsigned can_fifo_count(can_fifo_t *f)
{
    const unsigned tail = atomic_load_explicit(&f->tail, memory_order_acquire);
    const unsigned head = atomic_load_explicit(&f->head, memory_order_acquire);
    return head - tail;
}
