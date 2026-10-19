/*
 * port_hooks.c - FreeRTOS application hooks and HAL for the Cortex-M3 port.
 * (Fatal-error hooks live in fault.c.)
 */
#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bms_hal.h"
#include "mps2_an385.h"
#include "uart_cmsdk.h"

/* With static allocation the kernel asks us for the idle task's memory. */
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, configSTACK_DEPTH_TYPE *depth)
{
    static StaticTask_t idle_tcb;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];

    *tcb = &idle_tcb;
    *stack = idle_stack;
    *depth = configMINIMAL_STACK_SIZE;
}

/* The idle task runs only when no other task is ready. WFI stops the core
 * until the next interrupt (at the latest the next SysTick), instead of
 * burning cycles in a loop. Must never block. */
#ifndef BMS_IDLE_WFI
#define BMS_IDLE_WFI 1
#endif

void vApplicationIdleHook(void)
{
#if BMS_IDLE_WFI
    __asm volatile("wfi");
#endif
}

/* The AN385 has no relay drivers; on a real BMS board these would be GPIO
 * outputs driving the contactor coils through low-side drivers. Safe state
 * (all false) is also the reset state of a GPIO, so a reset opens all relays. */
static volatile bool s_contactor_neg, s_contactor_pre, s_contactor_pos;

void hal_contactors_set(bool neg, bool pre, bool pos)
{
    s_contactor_neg = neg;
    s_contactor_pre = pre;
    s_contactor_pos = pos;
}

void hal_console_write(const char *buf, size_t len)
{
    uart_write_polled(CMSDK_UART0, buf, len);
}

/* Request a system reset through the NVIC (the CMSIS NVIC_SystemReset
 * sequence). The VECTKEY must accompany every AIRCR write; PRIGROUP is
 * preserved. QEMU resets the whole machine and reloads the image. */
void hal_system_reset(void)
{
    __asm volatile("dsb" ::: "memory"); /* finish outstanding writes first */
    SCB_AIRCR = SCB_AIRCR_VECTKEY | (SCB_AIRCR & 0x700u) | SCB_AIRCR_SYSRESETREQ;
    __asm volatile("dsb" ::: "memory");
    for (;;) {
    }
}

/* Integer divide by zero. With CCR.DIV_0_TRP set in startup this raises a
 * UsageFault (escalated to HardFault) instead of quietly yielding 0. */
void hal_trigger_fault(void)
{
    volatile int numerator = 1;
    volatile int zero = 0;
    // cppcheck-suppress zerodiv ; deliberate: this is the fault being injected
    volatile int result = numerator / zero;
    (void)result;
}
