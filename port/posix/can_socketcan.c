/*
 * can_socketcan.c - can_if implementation over Linux SocketCAN (e.g. vcan0).
 *
 * ======================== The RX thread bridge ===========================
 *
 * Problem: reading a SocketCAN socket is a *blocking* Linux syscall. On the
 * FreeRTOS POSIX port every task is a pthread, but the kernel only lets one
 * of them run at a time and switches between them with signals. A task that
 * sits in read() is invisible to the scheduler - FreeRTOS thinks it is
 * running, so no other task gets the "CPU" until the read returns.
 *
 * Why not just call xQueueSend() from a separate pthread? Because on this
 * port FreeRTOS critical sections work by blocking signals *in the calling
 * thread* and portSET_INTERRUPT_MASK_FROM_ISR() is a no-op. A foreign
 * pthread calling into the kernel would mutate the ready/event lists
 * concurrently with a running task, with nothing to stop it. (port.c also
 * asserts if a non-FreeRTOS thread tries to yield.) The rule is: threads
 * that FreeRTOS did not create must never call the FreeRTOS API.
 *
 * Solution - model it like real hardware:
 *
 *   rx pthread (not a task)          tick "ISR" (SIGALRM)          CAN RX task
 *   ------------------------         ---------------------         -----------
 *   read() blocks on socket   --->   can_fifo (lock-free)   --->   FreeRTOS queue
 *   = CAN controller + DMA           xQueueSendFromISR()           xQueueReceive()
 *     filling an RX FIFO             = RX interrupt handler        with timeout
 *
 *  1. A plain pthread with ALL signals blocked does the blocking read() and
 *     pushes frames into a lock-free SPSC FIFO (C11 atomics, no locks, so it
 *     can never deadlock against a signal handler).
 *  2. vApplicationTickHook() runs inside the port's SIGALRM tick handler,
 *     which is the POSIX port's equivalent of an interrupt. There it is legal
 *     to call xQueueSendFromISR(), which moves frames into a normal FreeRTOS
 *     queue and unblocks the CAN RX task.
 *  3. The CAN RX task blocks on that queue with a timeout (can_if_receive).
 *
 * Cost: up to one tick (1 ms) of extra RX latency. The same FIFO + queue
 * shape will be reused on Cortex-M3, where the producer becomes the UART
 * RX ISR decoding slcan.
 * ========================================================================
 */
#define _GNU_SOURCE /* SocketCAN/ioctl under -std=c11, plus pthread_setname_np() */

#include "can_if.h"
#include "can_socketcan.h"
#include "can_fifo.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "FreeRTOS.h"
#include "queue.h"

#define CAN_RX_QUEUE_LEN       32u
#define CAN_MAX_FILTERS        16u
/* Bound the work done per tick interrupt; keeps the "ISR" short. */
#define CAN_MAX_FRAMES_PER_TICK 16u

static int s_sock = -1;
static pthread_t s_rx_thread;
static can_fifo_t s_fifo;

static StaticQueue_t s_rxq_struct;
static uint8_t s_rxq_storage[CAN_RX_QUEUE_LEN * sizeof(can_frame_t)];
static QueueHandle_t s_rxq;

static atomic_uint s_rx_overruns;   /* written by the RX pthread         */
static atomic_uint s_rx_frames;     /* written by the tick hook          */
static atomic_uint s_tx_frames;     /* written by the TX task            */
static atomic_uint s_tx_errors;

static void *rx_thread_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct can_frame raw;
        const ssize_t n = read(s_sock, &raw, sizeof raw);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* Socket gone (interface deleted?). Nothing sensible to do but
             * stop receiving; the BMS will raise a measurement timeout. */
            perror("can rx: read");
            return NULL;
        }
        if ((size_t)n != sizeof raw) {
            continue; /* not a classic CAN frame */
        }
        /* The kernel filter already passes only our standard data frames,
         * but stay defensive about extended/RTR/error frames. */
        if ((raw.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0u) {
            continue;
        }

        can_frame_t frame = {0};
        frame.id = raw.can_id & CAN_SFF_MASK;
        frame.dlc = (raw.can_dlc > CAN_MAX_DLC) ? (uint8_t)CAN_MAX_DLC : raw.can_dlc;
        memcpy(frame.data, raw.data, frame.dlc);

        if (!can_fifo_push(&s_fifo, &frame)) {
            atomic_fetch_add_explicit(&s_rx_overruns, 1u, memory_order_relaxed);
        }
    }
}

can_if_status_t can_if_init(const char *channel, const uint32_t *accept_ids, size_t num_ids)
{
    if (channel == NULL || num_ids > CAN_MAX_FILTERS || (num_ids > 0u && accept_ids == NULL)) {
        return CAN_IF_ERROR;
    }

    s_sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s_sock < 0) {
        perror("can: socket(PF_CAN)");
        return CAN_IF_ERROR;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, channel, IFNAMSIZ - 1);
    if (ioctl(s_sock, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "can: interface '%s' not found (run scripts/setup_vcan.sh): %s\n",
                channel, strerror(errno));
        close(s_sock);
        s_sock = -1;
        return CAN_IF_ERROR;
    }

    /* Acceptance filter: exact match on each standard ID, data frames only.
     * Done in the kernel so unwanted traffic never wakes our RX thread. */
    if (num_ids > 0u) {
        struct can_filter filters[CAN_MAX_FILTERS];
        for (size_t i = 0; i < num_ids; i++) {
            filters[i].can_id = accept_ids[i] & CAN_SFF_MASK;
            filters[i].can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
        }
        if (setsockopt(s_sock, SOL_CAN_RAW, CAN_RAW_FILTER, filters,
                       (socklen_t)(num_ids * sizeof filters[0])) < 0) {
            perror("can: CAN_RAW_FILTER");
            close(s_sock);
            s_sock = -1;
            return CAN_IF_ERROR;
        }
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof addr);
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("can: bind");
        close(s_sock);
        s_sock = -1;
        return CAN_IF_ERROR;
    }

    can_fifo_init(&s_fifo);
    s_rxq = xQueueCreateStatic(CAN_RX_QUEUE_LEN, sizeof(can_frame_t), s_rxq_storage, &s_rxq_struct);
    configASSERT(s_rxq != NULL);

    /* Create the RX thread with every signal blocked (it inherits the mask
     * of its creator). The port delivers its tick SIGALRM with pthread_kill()
     * to the running *task* thread, but blocking everything here guarantees
     * no signal handler - and so no FreeRTOS code - can ever run on this
     * thread, and that read() is never interrupted with EINTR. */
    sigset_t all, saved;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &saved);
    const int rc = pthread_create(&s_rx_thread, NULL, rx_thread_main, NULL);
    pthread_sigmask(SIG_SETMASK, &saved, NULL);
    if (rc != 0) {
        fprintf(stderr, "can: pthread_create: %s\n", strerror(rc));
        return CAN_IF_ERROR;
    }
    (void)pthread_setname_np(s_rx_thread, "can-rx-bridge");
    return CAN_IF_OK;
}

/*
 * Interrupt context (called from vApplicationTickHook inside the SIGALRM
 * handler, with all signals blocked). Only *FromISR APIs are allowed here.
 */
void can_socketcan_tick_hook(void)
{
    if (s_rxq == NULL) {
        return;
    }

    BaseType_t higher_prio_woken = pdFALSE;
    can_frame_t frame;
    unsigned moved = 0u;

    while (moved < CAN_MAX_FRAMES_PER_TICK && can_fifo_peek(&s_fifo, &frame)) {
        if (xQueueSendFromISR(s_rxq, &frame, &higher_prio_woken) != pdPASS) {
            break; /* queue full: leave the frame in the FIFO for next tick */
        }
        can_fifo_pop(&s_fifo);
        moved++;
    }
    if (moved > 0u) {
        atomic_fetch_add_explicit(&s_rx_frames, moved, memory_order_relaxed);
    }
    /* No portYIELD_FROM_ISR() here: we are inside xTaskIncrementTick(). If
     * the send unblocked a higher-priority task, the kernel has set
     * xYieldPending and the tick handler switches to it as it returns. */
    (void)higher_prio_woken;
}

can_if_status_t can_if_receive(can_frame_t *frame, uint32_t timeout_ms)
{
    if (frame == NULL || s_rxq == NULL) {
        return CAN_IF_ERROR;
    }
    if (xQueueReceive(s_rxq, frame, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        return CAN_IF_TIMEOUT;
    }
    return CAN_IF_OK;
}

can_if_status_t can_if_send(const can_frame_t *frame)
{
    if (frame == NULL || s_sock < 0 || frame->dlc > CAN_MAX_DLC || frame->id > CAN_SFF_MASK) {
        return CAN_IF_ERROR;
    }

    struct can_frame raw;
    memset(&raw, 0, sizeof raw);
    raw.can_id = frame->id;
    raw.can_dlc = frame->dlc;
    memcpy(raw.data, frame->data, frame->dlc);

    /* MSG_DONTWAIT: a real CAN driver returns "mailbox busy" rather than
     * blocking, and a blocked syscall would stall the scheduler here.
     * EINTR is possible because the port's SIGALRM handler is installed
     * without SA_RESTART. */
    for (;;) {
        const ssize_t n = send(s_sock, &raw, sizeof raw, MSG_DONTWAIT);
        if (n == (ssize_t)sizeof raw) {
            atomic_fetch_add_explicit(&s_tx_frames, 1u, memory_order_relaxed);
            return CAN_IF_OK;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        atomic_fetch_add_explicit(&s_tx_errors, 1u, memory_order_relaxed);
        return CAN_IF_ERROR; /* ENOBUFS/EAGAIN: TX queue full */
    }
}

void can_if_get_stats(can_if_stats_t *stats)
{
    stats->rx_frames = atomic_load_explicit(&s_rx_frames, memory_order_relaxed);
    stats->rx_overruns = atomic_load_explicit(&s_rx_overruns, memory_order_relaxed);
    stats->rx_errors = 0u; /* the kernel hands us whole frames: nothing to mis-parse */
    stats->tx_frames = atomic_load_explicit(&s_tx_frames, memory_order_relaxed);
    stats->tx_errors = atomic_load_explicit(&s_tx_errors, memory_order_relaxed);
}
