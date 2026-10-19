/*
 * port_hooks.c - FreeRTOS application hooks and HAL for the POSIX port.
 */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bms_hal.h"
#include "can_socketcan.h"

/* With configSUPPORT_STATIC_ALLOCATION the kernel asks us for the idle task's
 * memory instead of allocating it. */
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, configSTACK_DEPTH_TYPE *depth)
{
    static StaticTask_t idle_tcb;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];

    *tcb = &idle_tcb;
    *stack = idle_stack;
    *depth = configMINIMAL_STACK_SIZE;
}

/* Runs every tick inside the tick interrupt (SIGALRM handler on this port).
 * Must be short and may only use *FromISR APIs. */
void vApplicationTickHook(void)
{
    can_socketcan_tick_hook();
}

void vAssertCalled(const char *file, unsigned long line)
{
    /* fprintf from here is acceptable only because we abort right after. */
    fprintf(stderr, "ASSERT FAILED: %s:%lu\n", file, line);
    fflush(stderr);
    abort();
}

/* On the simulator the "relays" are just remembered; the contactor task
 * already logs every change. The Cortex-M3 port will drive GPIOs here. */
static volatile bool s_contactor_neg, s_contactor_pre, s_contactor_pos;

void hal_contactors_set(bool neg, bool pre, bool pos)
{
    s_contactor_neg = neg;
    s_contactor_pre = pre;
    s_contactor_pos = pos;
}

/* There is no "ECU" to reset on the simulator: end the process and let the
 * caller (the test harness, a shell loop) start a fresh one. */
void hal_system_reset(void)
{
    static const char msg[] = "system reset requested: exiting (POSIX port)\n";
    hal_console_write(msg, sizeof msg - 1u);
    exit(0);
}

/* The POSIX port has no fault handler to demonstrate; just say so. */
void hal_trigger_fault(void)
{
    static const char msg[] = "fault injection is only meaningful on the Cortex-M port\n";
    hal_console_write(msg, sizeof msg - 1u);
}

/* Raw write(2) instead of stdio: the port's tick signal is installed without
 * SA_RESTART, so a write can be interrupted (EINTR) or be partial. stdio
 * would just set its error flag and drop the text; here we retry. */
void hal_console_write(const char *buf, size_t len)
{
    while (len > 0u) {
        const ssize_t n = write(STDOUT_FILENO, buf, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return; /* stdout closed: nothing useful to do */
        }
        buf += n;
        len -= (size_t)n;
    }
}
