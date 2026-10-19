/*
 * fault.c - Fatal error handling: HardFault register dump, configASSERT,
 * stack overflow and malloc-failed hooks, and the no-heap tripwire.
 *
 * Everything here writes to the log UART by polling and never calls into the
 * RTOS for help: when we get here the scheduler, the heap or the stack may be
 * exactly what is broken.
 *
 * After the report the ECU resets, as production firmware must: the
 * bootloader runs again, and if the image that crashed is a trial image, the
 * crash has used up one of its boot attempts - a release that keeps crashing
 * is rolled back without anyone touching the vehicle. Build with
 * FATAL_HALT=1 to stop instead, so a debugger can inspect the wreckage
 * (make debug TARGET=qemu).
 */
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bms_hal.h"
#include "mps2_an385.h"
#include "uart_cmsdk.h"

#define CONSOLE CMSDK_UART0

#ifndef BMS_FATAL_HALT
#define BMS_FATAL_HALT 0
#endif

static void out(const char *s)
{
    uart_puts_polled(CONSOLE, s);
}

/* Last step of every fatal handler. Interrupts are already disabled. */
static void fatal_stop(void) __attribute__((noreturn));
static void fatal_stop(void)
{
#if BMS_FATAL_HALT
    out("Halted (FATAL_HALT build). Inspect with GDB: make debug TARGET=qemu\n");
    for (;;) {
    }
#else
    out("Resetting.\n");
    hal_system_reset();
    for (;;) { /* not reached */
    }
#endif
}

static void out_dec(unsigned long v)
{
    char buf[12];
    size_t i = sizeof buf;
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && i > 0u);
    uart_write_polled(CONSOLE, &buf[i], sizeof buf - i);
}

static void out_reg(const char *name, uint32_t value)
{
    out("  ");
    out(name);
    out(" = ");
    uart_puthex32_polled(CONSOLE, value);
    out("\n");
}

static void current_task_name(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        out(" (task '");
        out(pcTaskGetName(NULL)); /* reads the current TCB; no locking needed */
        out("')");
    }
}

/* ---- HardFault ------------------------------------------------------------ */

typedef struct {
    uint32_t bit;
    const char *what;
} fault_bit_t;

/* CFSR = UFSR[31:16] | BFSR[15:8] | MMFSR[7:0] */
static const fault_bit_t s_cfsr_bits[] = {
    { 1u << 0,  "MemManage: instruction fetch from a no-execute region (IACCVIOL)" },
    { 1u << 1,  "MemManage: data access violation (DACCVIOL)" },
    { 1u << 3,  "MemManage: fault while unstacking on exception return (MUNSTKERR)" },
    { 1u << 4,  "MemManage: fault while stacking on exception entry (MSTKERR)" },
    { 1u << 8,  "BusFault: instruction fetch bus error (IBUSERR)" },
    { 1u << 9,  "BusFault: precise data bus error, BFAR holds the address (PRECISERR)" },
    { 1u << 10, "BusFault: imprecise data bus error, PC is after the culprit (IMPRECISERR)" },
    { 1u << 11, "BusFault: fault while unstacking (UNSTKERR)" },
    { 1u << 12, "BusFault: fault while stacking - often a stack overflow (STKERR)" },
    { 1u << 16, "UsageFault: undefined instruction (UNDEFINSTR)" },
    { 1u << 17, "UsageFault: invalid state, e.g. branch to an even (ARM, not Thumb) address (INVSTATE)" },
    { 1u << 18, "UsageFault: invalid EXC_RETURN on exception return (INVPC)" },
    { 1u << 19, "UsageFault: coprocessor access - there is no FPU on an M3 (NOCP)" },
    { 1u << 24, "UsageFault: unaligned access (UNALIGNED)" },
    { 1u << 25, "UsageFault: integer divide by zero (DIVBYZERO)" },
};

/* Called from the naked HardFault_Handler with the exception stack frame.
 * Not static: the assembly trampoline branches to it by name. */
void fault_report(const uint32_t *frame, uint32_t exc_return) __attribute__((used, noreturn));
void fault_report(const uint32_t *frame, uint32_t exc_return)
{
    const uint32_t cfsr = SCB_CFSR;
    const uint32_t hfsr = SCB_HFSR;

    out("\n*** HARD FAULT");
    if ((exc_return & 0x4u) != 0u) {
        current_task_name(); /* frame was on the PSP: a task faulted */
    } else {
        out(" (handler mode / MSP)");
    }
    out(" ***\n");

    /* On exception entry the core pushes these 8 registers itself, so they
     * are the CPU state at the faulting instruction. PC is the instruction
     * that faulted (for precise faults); LR is where its function was called
     * from. Decode with: arm-none-eabi-addr2line -e build/qemu/app_slotA.elf <PC> */
    out_reg("R0  ", frame[0]);
    out_reg("R1  ", frame[1]);
    out_reg("R2  ", frame[2]);
    out_reg("R3  ", frame[3]);
    out_reg("R12 ", frame[4]);
    out_reg("LR  ", frame[5]);
    out_reg("PC  ", frame[6]);
    out_reg("xPSR", frame[7]);
    out_reg("SP  ", (uint32_t)frame + 32u); /* SP before the exception pushed the frame */
    out_reg("EXC_RETURN", exc_return);
    out_reg("CFSR", cfsr);
    out_reg("HFSR", hfsr);
    if ((cfsr & SCB_CFSR_MMARVALID) != 0u) {
        out_reg("MMFAR", SCB_MMFAR);
    }
    if ((cfsr & SCB_CFSR_BFARVALID) != 0u) {
        out_reg("BFAR", SCB_BFAR);
    }

    if ((hfsr & SCB_HFSR_FORCED) != 0u) {
        out("  cause: escalated configurable fault (HFSR.FORCED):\n");
    }
    if ((hfsr & SCB_HFSR_VECTTBL) != 0u) {
        out("  cause: bus fault reading the vector table - check VTOR\n");
    }
    for (size_t i = 0; i < sizeof s_cfsr_bits / sizeof s_cfsr_bits[0]; i++) {
        if ((cfsr & s_cfsr_bits[i].bit) != 0u) {
            out("    - ");
            out(s_cfsr_bits[i].what);
            out("\n");
        }
    }
    fatal_stop();
}

/*
 * Exception entry pushes R0-R3, R12, LR, PC, xPSR onto whichever stack was in
 * use: the PSP if a task was running (FreeRTOS tasks use the process stack),
 * the MSP if an interrupt handler or pre-scheduler code was. Bit 2 of the
 * EXC_RETURN value in LR says which. Naked, because a C prologue would push
 * more onto the stack before we could look at it.
 */
void HardFault_Handler(void);
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst   lr, #4          \n\t"
        "ite   eq              \n\t"
        "mrseq r0, msp         \n\t"
        "mrsne r0, psp         \n\t"
        "mov   r1, lr          \n\t"
        "b     fault_report    \n\t");
}

/* ---- FreeRTOS hooks ----------------------------------------------------- */

void vAssertCalled(const char *file, unsigned long line)
{
    taskDISABLE_INTERRUPTS();
    out("\n*** ASSERT FAILED: ");
    out(file);
    out(":");
    out_dec(line);
    current_task_name();
    out("\n");
    fatal_stop();
}

/* configCHECK_FOR_STACK_OVERFLOW = 2: on every context switch the kernel
 * checks that the stack pointer is in range (method 1) AND that the last 16
 * bytes of the stack still hold the fill pattern (method 2). It catches most
 * overflows, but only after the fact - the overflow has already corrupted
 * whatever lies below the stack, so the only safe reaction is to stop. */
// cppcheck-suppress constParameterPointer ; signature is fixed by FreeRTOS task.h
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    taskDISABLE_INTERRUPTS();
    out("\n*** STACK OVERFLOW in task '");
    out(name);
    out("'\n");
    fatal_stop();
}

/* FreeRTOS calls this when pvPortMalloc() fails. This firmware has no
 * FreeRTOS heap at all (static allocation only), so it is wired to the one
 * place left where a hidden allocation could still happen: the C library's
 * malloc, via _sbrk() below. */
void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    out("\n*** MALLOC FAILED: something tried to allocate heap memory - this firmware has none\n");
    fatal_stop();
}

/* newlib's malloc grows its heap by calling _sbrk(). We refuse: "no dynamic
 * allocation after startup" is enforced at runtime, not just by convention.
 * (snprintf's string-output path links realloc but never calls it.) */
void *_sbrk(ptrdiff_t increment);
void *_sbrk(ptrdiff_t increment)
{
    extern char __heap_start; /* linker script */
    if (increment == 0) {
        return &__heap_start; /* malloc probes the current break first */
    }
    vApplicationMallocFailedHook();
    return (void *)-1;
}
