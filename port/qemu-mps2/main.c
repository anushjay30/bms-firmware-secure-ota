/*
 * main.c - Cortex-M3 (QEMU mps2-an385) entry point.
 *
 * By the time main() runs, the bootloader has verified this image and pointed
 * VTOR at our vector table, and Reset_Handler has set the stack pointer and
 * initialised .data/.bss.
 *
 *   UART0  human-readable log (polled)
 *   UART1  CAN frames as slcan ASCII (interrupt-driven); slcand on the host
 *          turns it into the SocketCAN interface slcan0
 */
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bms_app.h"
#include "bms_log.h"
#include "bms_port_config.h"
#include "memory_map.h"
#include "mps2_an385.h"
#include "port_image.h"
#include "uart_cmsdk.h"

/* QEMU implements all 8 NVIC priority bits; real M3 silicon usually 3 or 4.
 * Logged so the difference is visible; FreeRTOS checks it again at start. */
static unsigned implemented_priority_bits(void)
{
    NVIC_IPR[MPS2_IRQ_COUNT - 1] = 0xFFu; /* unused interrupt: write all ones */
    uint8_t v = NVIC_IPR[MPS2_IRQ_COUNT - 1];
    NVIC_IPR[MPS2_IRQ_COUNT - 1] = 0u;
    unsigned bits = 0u;
    while ((v & 0x80u) != 0u) {
        bits++;
        v = (uint8_t)(v << 1);
    }
    return bits;
}

int main(void);
int main(void)
{
    uart_init_polled(CMSDK_UART0, BMS_UART_BAUD);

    const uint32_t version = port_running_image()->image_version; /* as signed, not as linked */
    bms_log("MAIN", "BMS firmware starting: FreeRTOS %s, Cortex-M3 (QEMU mps2-an385), CAN = slcan on UART1",
            tskKERNEL_VERSION_NUMBER);
    bms_log("MAIN", "image v%lu.%lu.%lu in slot %c, vector table 0x%08lx, VTOR 0x%08lx, %u NVIC priority bits",
            (unsigned long)(version >> 16), (unsigned long)((version >> 8) & 0xFFu), (unsigned long)(version & 0xFFu),
            port_running_slot_name(), (unsigned long)MM_APP_VECTORS, (unsigned long)SCB_VTOR,
            implemented_priority_bits());

    if (!bms_app_init("uart1")) {
        bms_log("MAIN", "initialisation failed");
        for (;;) {
        }
    }

    vTaskStartScheduler();

    /* Only reached if the scheduler could not start (it never returns). */
    bms_log("MAIN", "scheduler exited");
    for (;;) {
    }
}
