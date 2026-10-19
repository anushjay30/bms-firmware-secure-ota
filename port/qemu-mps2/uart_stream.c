/*
 * uart_stream.c - Interrupt-driven byte stream on UART1 (the slcan CAN link).
 *
 * The CMSDK UART has no FIFO: one byte of receive buffer and one byte of
 * transmit buffer. That shapes the design below - the receive interrupt must
 * empty the 1-byte buffer before the next byte arrives (~87 us at 115200
 * baud), so the ISR does nothing but move bytes into a stream buffer; the slow
 * work (parsing slcan lines) happens in a task.
 */
#include "uart_stream.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "stream_buffer.h"

#include "bms_port_config.h"
#include "mps2_an385.h"

#define UART1_RX_BUF_SIZE 1024u /* ~45 slcan frames: covers a 10 ms burst with margin */
#define UART1_TX_BUF_SIZE 1024u

static StaticStreamBuffer_t s_rx_sb_struct;
static uint8_t s_rx_storage[UART1_RX_BUF_SIZE];
static StreamBufferHandle_t s_rx_sb;

static StaticStreamBuffer_t s_tx_sb_struct;
static uint8_t s_tx_storage[UART1_TX_BUF_SIZE];
static StreamBufferHandle_t s_tx_sb;

static StaticSemaphore_t s_tx_mutex_buf;
static SemaphoreHandle_t s_tx_mutex;

/* Each counter has exactly one writer (an ISR), and aligned 32-bit loads and
 * stores are single instructions on Cortex-M3, so readers never see a torn
 * value without any locking. */
static volatile uint32_t s_rx_bytes;
static volatile uint32_t s_rx_dropped;
static volatile uint32_t s_rx_hw_overruns;
static volatile uint32_t s_tx_bytes;

void uart1_stream_init(uint32_t baud)
{
    /* Trigger level 1: a blocked reader wakes as soon as any byte arrives.
     * Higher levels would batch wake-ups but delay the tail of a line. */
    s_rx_sb = xStreamBufferCreateStatic(sizeof s_rx_storage, 1u, s_rx_storage, &s_rx_sb_struct);
    s_tx_sb = xStreamBufferCreateStatic(sizeof s_tx_storage, 1u, s_tx_storage, &s_tx_sb_struct);
    s_tx_mutex = xSemaphoreCreateMutexStatic(&s_tx_mutex_buf);
    configASSERT(s_rx_sb != NULL && s_tx_sb != NULL && s_tx_mutex != NULL);

    cmsdk_uart_t *const u = CMSDK_UART1;
    u->CTRL = 0u;
    u->BAUDDIV = MPS2_SYSCLK_HZ / baud;
    u->STATE = UART_STATE_RXOVR | UART_STATE_TXOVR; /* write-1-to-clear stale flags */
    u->INTSTATUS = UART_INT_RX | UART_INT_TX;       /* (INTCLEAR) */

    /*
     * Interrupt priority: these ISRs call FreeRTOS *FromISR() functions, so
     * they must be at or below configMAX_SYSCALL_INTERRUPT_PRIORITY in
     * urgency (numerically >=). FreeRTOS critical sections only mask up to
     * that level via BASEPRI; an ISR above it could interrupt the kernel in
     * the middle of updating a list. The reset default priority is 0 - the
     * most urgent - which is the classic FreeRTOS-on-Cortex-M bug; with
     * configASSERT on, vPortValidateInterruptPriority() catches it.
     */
    nvic_set_priority(UART1_RX_IRQn, BMS_IRQ_PRIO_UART);
    nvic_set_priority(UART1_TX_IRQn, BMS_IRQ_PRIO_UART);
    nvic_clear_pending(UART1_RX_IRQn);
    nvic_clear_pending(UART1_TX_IRQn);
    nvic_enable_irq(UART1_RX_IRQn);
    nvic_enable_irq(UART1_TX_IRQn);

    /* Until vTaskStartScheduler() runs, FreeRTOS keeps BASEPRI raised (it
     * did so in the first queue create), so these interrupts cannot fire
     * before there is a task to switch to. */
    u->CTRL = UART_CTRL_TXEN | UART_CTRL_RXEN | UART_CTRL_RXIE | UART_CTRL_TXIE;
}

/* Receive interrupt: move every waiting byte into the stream buffer. */
void UART1_RX_IRQHandler(void);
void UART1_RX_IRQHandler(void)
{
    cmsdk_uart_t *const u = CMSDK_UART1;
    BaseType_t woken = pdFALSE;

    /* Clear the interrupt flag FIRST, then drain. If a byte lands after our
     * last RXFULL check it sets the flag again and we are re-entered. Clearing
     * after draining could wipe that new byte's interrupt: the byte would sit
     * in the 1-byte buffer forever (and QEMU, which holds further bytes back
     * while the buffer is full, would stall the link completely). */
    u->INTSTATUS = UART_INT_RX;
    while ((u->STATE & UART_STATE_RXFULL) != 0u) {
        const uint8_t byte = (uint8_t)u->DATA; /* reading DATA frees the buffer */
        if (xStreamBufferSendFromISR(s_rx_sb, &byte, 1u, &woken) == 1u) {
            s_rx_bytes++;
        } else {
            s_rx_dropped++; /* task not keeping up; the slcan parser resyncs */
        }
    }
    if ((u->STATE & UART_STATE_RXOVR) != 0u) {
        /* Real silicon: a byte was lost because we were too slow. (QEMU
         * applies back-pressure instead, so this never fires there.) */
        u->STATE = UART_STATE_RXOVR;
        s_rx_hw_overruns++;
    }
    /* If the send unblocked the CAN RX task, switch to it as soon as we
     * return instead of resuming whatever task was interrupted. */
    portYIELD_FROM_ISR(woken);
}

/* Transmit interrupt: feed the UART from the stream buffer. Also invoked by
 * software (NVIC set-pending) to start transmission, so the ISR is the only
 * reader of the TX stream buffer. */
void UART1_TX_IRQHandler(void);
void UART1_TX_IRQHandler(void)
{
    cmsdk_uart_t *const u = CMSDK_UART1;
    BaseType_t woken = pdFALSE;

    u->INTSTATUS = UART_INT_TX; /* clear first, same reasoning as RX */
    while ((u->STATE & UART_STATE_TXFULL) == 0u) {
        uint8_t byte;
        if (xStreamBufferReceiveFromISR(s_tx_sb, &byte, 1u, &woken) != 1u) {
            break; /* nothing left: the next write kicks us again */
        }
        u->DATA = byte;
        s_tx_bytes++;
    }
    portYIELD_FROM_ISR(woken);
}

size_t uart1_stream_read(uint8_t *buf, size_t len, uint32_t timeout_ticks)
{
    return xStreamBufferReceive(s_rx_sb, buf, len, (TickType_t)timeout_ticks);
}

bool uart1_stream_write(const uint8_t *buf, size_t len)
{
    bool queued = false;

    /* Stream buffers allow one writer at a time. Two tasks write here (frames
     * from CAN TX, protocol replies from CAN RX), so serialise them. The
     * mutex is held only for a non-blocking copy of <= 32 bytes, so a waiting
     * task is delayed by microseconds at worst (and priority inheritance
     * bounds it). */
    (void)xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    if (xStreamBufferSpacesAvailable(s_tx_sb) >= len) {
        queued = xStreamBufferSend(s_tx_sb, buf, len, 0) == len;
    }
    (void)xSemaphoreGive(s_tx_mutex);

    if (queued) {
        /* Kick the TX interrupt. If a byte is already in flight it simply
         * finds TXFULL set and leaves; that byte's completion interrupt then
         * continues with ours. */
        nvic_set_pending(UART1_TX_IRQn);
    }
    return queued;
}

void uart1_stream_get_stats(uart_stream_stats_t *stats)
{
    stats->rx_bytes = s_rx_bytes;
    stats->rx_dropped = s_rx_dropped;
    stats->rx_hw_overruns = s_rx_hw_overruns;
    stats->tx_bytes = s_tx_bytes;
}
