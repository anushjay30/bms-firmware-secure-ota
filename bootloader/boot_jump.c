/*
 * boot_jump.c - Hand the CPU over to the application.
 *
 * Goal: start the application in a state as close as possible to a real
 * reset, so it cannot tell - and need not care - that a bootloader ran first.
 * Anything the bootloader configures and leaves behind is a bug waiting to
 * appear only after an update, so each step below undoes or replaces one
 * piece of CPU state.
 *
 * This bootloader polls everything and never enables an interrupt or
 * SysTick, so steps 2 and 3 are defensive: they keep the hand-over correct
 * if it ever grows a feature that does (a CAN recovery mode, a timeout), for
 * the cost of a few instructions. Not touched because the bootloader never
 * changes them from their reset values: CONTROL (privileged, MSP), the MPU
 * (off), BASEPRI/FAULTMASK (0).
 */
#include "boot_jump.h"

#include "mps2_an385.h"

void boot_jump_to_app(uint32_t vector_table)
{
    const volatile uint32_t *vt = (const volatile uint32_t *)vector_table;
    const uint32_t app_msp = vt[0];   /* entry 0: initial main stack pointer          */
    const uint32_t app_reset = vt[1]; /* entry 1: reset handler, bit 0 set (Thumb)    */

    /* 1. Disable interrupts (PRIMASK = 1). Until the application has its own
     *    vector table and handlers in place, an interrupt would be taken
     *    through a table that is being swapped underneath it. PRIMASK survives
     *    a branch (only a real reset clears it), so the application starts
     *    with interrupts masked and enables them itself: FreeRTOS does so
     *    when it starts the first task. */
    __asm volatile("cpsid i" ::: "memory");

    /* 2. Stop SysTick and clear its state. Left running, its exception would
     *    fire the moment the application unmasks interrupts - before FreeRTOS
     *    has configured the timer - into a tick handler that expects a running
     *    scheduler. A write to CVR clears both the counter and COUNTFLAG. */
    SYST_CSR = 0u;
    SYST_RVR = 0u;
    SYST_CVR = 0u;

    /* 3. Disable every external interrupt and discard anything pending: an
     *    interrupt enabled or latched now would be delivered to the
     *    application as soon as it unmasks interrupts, possibly to the handler
     *    of a peripheral it has not initialised yet. The AN385 has 32 IRQs, so
     *    one clear-enable and one clear-pending register cover them all. Then
     *    the two system exceptions that can be pending without an enable bit:
     *    SysTick and PendSV. */
    NVIC_ICER0 = 0xFFFFFFFFu;
    NVIC_ICPR0 = 0xFFFFFFFFu;
    SCB_ICSR = SCB_ICSR_PENDSTCLR | SCB_ICSR_PENDSVCLR;

    /* 4. Point VTOR at the application's vector table, so that every
     *    exception from now on - including a fault in the next few
     *    instructions - goes to the application's handlers, not ours. (The
     *    table must be aligned to its size rounded up to a power of two,
     *    256 bytes here, which is why images put it at slot + 0x100.) DSB
     *    waits for the write to complete; ISB flushes the pipeline so the
     *    instructions after it run with the new setting. */
    SCB_VTOR = vector_table;
    __asm volatile("dsb\n\tisb" ::: "memory");

    /* 5. Load the application's initial MSP, and 6. branch to its reset
     *    handler. A hardware reset takes both from the vector table; a branch
     *    does neither, so we do it here. This has to be one asm block: once
     *    MSP has changed, any stack access the compiler might insert (a spill,
     *    a saved return address) would land on the application's stack. BX to
     *    an address with bit 0 set stays in Thumb state; the image check has
     *    already rejected a reset vector without it. */
    __asm volatile(
        "msr  msp, %0 \n\t"
        "bx   %1      \n\t"
        :
        : "r"(app_msp), "r"(app_reset)
        : "memory");
    __builtin_unreachable();
}
