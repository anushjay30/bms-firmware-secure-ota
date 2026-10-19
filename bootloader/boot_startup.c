/*
 * boot_startup.c - Vector table, reset and fault handlers of the bootloader.
 *
 * This is the code the core runs out of reset. VTOR is 0 after reset, so the
 * table below - linked at 0x00000000 - supplies the initial stack pointer
 * (entry 0, loaded by the hardware before the first instruction, which is
 * why the reset handler can be plain C) and the reset vector. The
 * bootloader polls everything and never enables an interrupt, so apart from
 * reset only the fault vectors matter.
 */
#include <stddef.h>
#include <stdint.h>

#include "boot_log.h"
#include "mps2_an385.h"

typedef void (*vector_t)(void);

/* Linker script symbols (bootloader.ld.S), declared as arrays of unknown
 * size so that indexing from them is well-defined C. */
extern uint32_t __data_start[], __data_end[], __data_load[];
extern uint32_t __bss_start[], __bss_end[];
extern uint32_t __stack_top[];

void boot_main(void) __attribute__((noreturn));
void boot_reset_handler(void) __attribute__((noreturn));
void boot_fault_handler(void);

__attribute__((section(".boot_vectors"), used))
static const vector_t s_vectors[16] = {
    (vector_t)__stack_top, /*  0 initial MSP                 */
    boot_reset_handler,    /*  1 reset                       */
    boot_fault_handler,    /*  2 NMI                         */
    boot_fault_handler,    /*  3 HardFault                   */
    boot_fault_handler,    /*  4 MemManage                   */
    boot_fault_handler,    /*  5 BusFault                    */
    boot_fault_handler,    /*  6 UsageFault                  */
    0, 0, 0, 0,            /*  7-10 reserved                 */
    boot_fault_handler,    /* 11 SVCall     (never used)     */
    boot_fault_handler,    /* 12 DebugMonitor                */
    0,                     /* 13 reserved                    */
    boot_fault_handler,    /* 14 PendSV     (never used)     */
    boot_fault_handler,    /* 15 SysTick    (never enabled)  */
    /* No external interrupt vectors: the bootloader enables none. */
};

void boot_reset_handler(void)
{
    /* Initial values of .data from BOOT into RAM, then zero .bss. (Sizes on
     * integers: each linker symbol is a separate object to the compiler.) */
    const size_t data_words = ((uintptr_t)__data_end - (uintptr_t)__data_start) / sizeof(uint32_t);
    const size_t bss_words = ((uintptr_t)__bss_end - (uintptr_t)__bss_start) / sizeof(uint32_t);
    for (size_t i = 0; i < data_words; i++) {
        __data_start[i] = __data_load[i];
    }
    for (size_t i = 0; i < bss_words; i++) {
        __bss_start[i] = 0u;
    }

    SCB_CCR |= SCB_CCR_DIV_0_TRP; /* integer divide by zero traps instead of returning 0 */
    boot_main();
}

/* A fault in the bootloader is a bug in the bootloader, not a bad image:
 * report it and stop where a debugger can look (make debug TARGET=qemu).
 * Resetting would only loop through the same fault. */
void boot_fault_handler(void)
{
    boot_log_init(); /* the fault may have hit before the UART was set up */
    boot_printf("\nBOOT  FATAL: exception %u in the bootloader (CFSR %x, HFSR %x) - halted\n",
                (uint32_t)(SCB_ICSR & SCB_ICSR_VECTACTIVE_MASK), (uint32_t)SCB_CFSR, (uint32_t)SCB_HFSR);
    for (;;) {
    }
}
