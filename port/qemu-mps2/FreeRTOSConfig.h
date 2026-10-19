/*
 * FreeRTOSConfig.h - Kernel configuration for the Cortex-M3 (QEMU mps2-an385).
 *
 * Compare port/posix/FreeRTOSConfig.h: the application code is identical,
 * only this file and bms_port_config.h differ between the two targets.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ---- Scheduler ---------------------------------------------------------- */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
/* The M3 has a CLZ (count leading zeros) instruction, so the scheduler can
 * find the highest ready priority in one instruction instead of a loop. */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      25000000UL /* SysTick runs from the 25 MHz core clock */
/* SysTick is a real hardware counter here, so a 1 kHz tick is exact (the
 * POSIX port's usleep()-driven tick could not keep up with that). */
#define configTICK_RATE_HZ                      1000
#define configMAX_PRIORITIES                    8
#define configMAX_TASK_NAME_LEN                 16
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1

/* ---- Memory: static allocation only ------------------------------------- */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        0
#define configKERNEL_PROVIDED_STATIC_MEMORY     0 /* we supply the idle task's memory */
/* Idle task stack, in 32-bit words. It runs the idle hook (WFI) and nothing
 * else; the startup stack report shows the real usage. */
#define configMINIMAL_STACK_SIZE                ((unsigned short)128)

/* ---- Features ------------------------------------------------------------ */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_STREAM_BUFFERS                1 /* UART1 RX/TX */
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_TIMERS                        0
#define configUSE_CO_ROUTINES                   0
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configUSE_TRACE_FACILITY                0
#define configGENERATE_RUN_TIME_STATS           0

/* ---- Hooks and run-time checks ------------------------------------------- */
/* Idle hook executes WFI: the core sleeps until the next interrupt instead
 * of spinning (on hardware that saves power; under QEMU it frees host CPU). */
#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1 /* see fault.c: wired to _sbrk() */
#define configCHECK_FOR_STACK_OVERFLOW          2
/* Verify at scheduler start that the vector table (found via VTOR) really
 * contains the FreeRTOS SVC and PendSV handlers. Catches a bootloader that
 * forgot to set VTOR before jumping to us. */
#define configCHECK_HANDLER_INSTALLATION        1

/* ---- Interrupt priorities ------------------------------------------------
 * The NVIC on this M3 implements 3 priority bits (8 levels, in the top bits
 * of each 8-bit priority field). Lower number = more urgent.
 *
 *   0..4  never masked by FreeRTOS - may NOT call any FreeRTOS function
 *   5     configMAX_SYSCALL_INTERRUPT_PRIORITY: critical sections set BASEPRI
 *         to this, masking 5..7 while the kernel updates its lists
 *   5..7  may call *FromISR() functions (our UART ISRs use 6)
 *   7     lowest: PendSV and SysTick (the port forces 255 = lowest itself)
 *
 * Masking by BASEPRI rather than disabling everything (PRIMASK) is what lets
 * an urgent interrupt (e.g. an overcurrent comparator) stay zero-latency even
 * while the kernel is in a critical section.
 */
#define configPRIO_BITS                              3
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configMAX_SYSCALL_INTERRUPT_PRIORITY         (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* ---- API inclusion ------------------------------------------------------- */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskSuspend                    1 /* makes portMAX_DELAY mean "forever" */
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_vTaskDelete                     0

/* ---- Assertions ---------------------------------------------------------- */
void vAssertCalled(const char *file, unsigned long line);
#define configASSERT(x)                         \
    do {                                        \
        if ((x) == 0) {                         \
            vAssertCalled(__FILE__, __LINE__);  \
        }                                       \
    } while (0)

#endif /* FREERTOS_CONFIG_H */
