/*
 * startup.c - Vector table and reset handler of the BMS application.
 *
 * Cortex-M3 boot facts this file relies on:
 *  - The vector table is an array of addresses. Entry 0 is NOT code: it is
 *    the initial Main Stack Pointer, which the core loads into MSP before
 *    executing a single instruction. Entry 1 is the reset handler. That is
 *    why a Cortex-M reset handler can be plain C - unlike most CPUs, the
 *    stack already exists when the first instruction runs.
 *  - Every handler address must have bit 0 set (Thumb). The M3 only executes
 *    Thumb code; branching to an even address raises a UsageFault (INVSTATE).
 *    The compiler/linker set bit 0 for us when we take a function's address.
 *  - The core finds the table through VTOR, which resets to 0x00000000 -
 *    the bootloader's table. Our table is at slot + 0x100, so the bootloader
 *    points VTOR here before jumping, and we set it again ourselves first
 *    thing rather than depend on that.
 *  - The bootloader hands over with interrupts disabled (PRIMASK = 1).
 *    Nothing before the scheduler needs them; FreeRTOS enables them when it
 *    starts the first task (cpsie i in the port's start-first-task code).
 */
#include <stddef.h>
#include <stdint.h>

#include "memory_map.h"
#include "mps2_an385.h"

typedef void (*vector_t)(void);

/* Linker script symbols: addresses only, no storage of their own. Declared
 * as arrays of unknown size so indexing from them is well-defined C. */
extern uint32_t __data_start[], __data_end[], __data_load[];
extern uint32_t __bss_start[], __bss_end[];
extern uint32_t __msp_stack_top[];

extern int main(void);

/* FreeRTOS Cortex-M3 port handlers. Installed directly (rather than renamed
 * with #defines) so configCHECK_HANDLER_INSTALLATION can verify them. */
void vPortSVCHandler(void);
void xPortPendSVHandler(void);
void xPortSysTickHandler(void);

void Reset_Handler(void);
void HardFault_Handler(void); /* fault.c: prints the stacked registers */
void Default_Handler(void);
void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));

/* Peripheral handlers default to Default_Handler; a driver overrides one just
 * by defining a function with the same name (weak alias). */
void UART0_RX_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void UART0_TX_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void UART1_RX_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void UART1_TX_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

#define D Default_Handler

__attribute__((section(".isr_vector"), used))
const vector_t g_vectors[16 + MPS2_IRQ_COUNT] = {
    /* ---- System exceptions (vector 0..15) ---- */
    (vector_t)__msp_stack_top,  /*  0 initial MSP, loaded by hardware at reset */
    Reset_Handler,              /*  1 */
    NMI_Handler,                /*  2 */
    HardFault_Handler,          /*  3 */
    /* MemManage, BusFault and UsageFault are left disabled in SHCSR, so they
     * escalate to HardFault, whose handler decodes the cause from CFSR. */
    HardFault_Handler,          /*  4 MemManage  */
    HardFault_Handler,          /*  5 BusFault   */
    HardFault_Handler,          /*  6 UsageFault */
    0, 0, 0, 0,                 /*  7-10 reserved */
    vPortSVCHandler,            /* 11 SVCall: FreeRTOS starts the first task */
    D,                          /* 12 DebugMonitor */
    0,                          /* 13 reserved */
    xPortPendSVHandler,         /* 14 PendSV: context switch, lowest priority */
    xPortSysTickHandler,        /* 15 SysTick: 1 kHz RTOS tick */
    /* ---- External interrupts IRQ0..31 (vector 16..47) ---- */
    UART0_RX_IRQHandler,        /* IRQ0 */
    UART0_TX_IRQHandler,        /* IRQ1 */
    UART1_RX_IRQHandler,        /* IRQ2  CAN (slcan) receive  */
    UART1_TX_IRQHandler,        /* IRQ3  CAN (slcan) transmit */
    D, D, D, D,                 /* IRQ4-7   UART2, GPIO */
    D, D, D, D,                 /* IRQ8-11  timers, ... */
    D, D, D, D,                 /* IRQ12-15 UART overrun, ... */
    D, D, D, D, D, D, D, D,     /* IRQ16-23 */
    D, D, D, D, D, D, D, D,     /* IRQ24-31 */
};

#undef D

/* C part of the reset sequence. Runs before .data/.bss are initialised, so it
 * must not depend on any global variable having its initial value. */
__attribute__((noreturn, used)) static void reset_c(void)
{
    /* Point VTOR at our table. Until this write, exceptions would use the
     * table of whoever started us. DSB+ISB make the new table take effect
     * before anything (an exception, the next instruction) can use it. */
    SCB_VTOR = (uint32_t)g_vectors;
    __asm volatile("dsb\n\tisb" ::: "memory");

    /* Copy initial values of .data from the image (LMA) into RAM (VMA), then
     * zero .bss. On the real board RAM is random at power-up; QEMU happens to
     * pre-load .data, which is exactly the kind of difference that hides
     * bugs until the first hardware bring-up.
     * Sizes are computed on integers: to the C compiler each linker symbol is
     * a separate object, and comparing pointers to different objects is
     * undefined behaviour that an optimiser is allowed to exploit. */
    const size_t data_words = ((uintptr_t)__data_end - (uintptr_t)__data_start) / sizeof(uint32_t);
    const size_t bss_words = ((uintptr_t)__bss_end - (uintptr_t)__bss_start) / sizeof(uint32_t);

    for (size_t i = 0; i < data_words; i++) {
        __data_start[i] = __data_load[i];
    }
    for (size_t i = 0; i < bss_words; i++) {
        __bss_start[i] = 0u;
    }

    /* Make integer divide-by-zero trap (UsageFault -> HardFault) instead of
     * silently returning 0. */
    SCB_CCR |= SCB_CCR_DIV_0_TRP;

    (void)main();
    for (;;) {
    }
}

/*
 * Reset entry. When the CPU itself resets into this image, MSP is already
 * loaded from g_vectors[0]. But a bootloader *branches* here, and nothing
 * reloads MSP on a branch - so set it explicitly. This has to be a naked
 * function: a normal C function would already be using the old stack in its
 * prologue when we swap it out from under it.
 */
__attribute__((naked, noreturn)) void Reset_Handler(void)
{
    __asm volatile(
        "ldr r0, =__msp_stack_top \n\t"
        "msr msp, r0              \n\t"
        "b   reset_c              \n\t"
        ".ltorg                   \n\t");
}

/* Unexpected interrupt: stop where a debugger can see which one it was
 * (ICSR.VECTACTIVE holds the vector number). */
void Default_Handler(void)
{
    volatile uint32_t active_vector = SCB_ICSR & SCB_ICSR_VECTACTIVE_MASK;
    (void)active_vector;
    for (;;) {
    }
}
