/*
 * FreeRTOSConfig.h - Kernel configuration for the POSIX (Linux) simulator port.
 *
 * Each port has its own copy of this file; app/ code never includes it directly
 * except through FreeRTOS.h, so the QEMU Cortex-M3 port can choose different
 * values (tick type width, priorities bits, stack sizes) without touching app/.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ---- Scheduler ---------------------------------------------------------- */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      1000000UL /* unused by the POSIX port */
/* 100 Hz (10 ms) tick on the simulator. The POSIX port generates ticks from
 * a helper thread that calls usleep(tick period) with no drift correction,
 * so every tick runs slow by the signal + context-switch overhead. At 1 kHz
 * that overhead is a large fraction of the period and simulated time runs
 * noticeably slower than the wall clock (we measured 17-96 % in a VM); at
 * 100 Hz it is ~1 %. Every BMS period is a multiple of 10 ms (checked by
 * static asserts in bms_app.c). A real Cortex-M SysTick is an exact hardware
 * timer, so that port can use 1 kHz. */
#define configTICK_RATE_HZ                      100
#define configMAX_PRIORITIES                    8
#define configMAX_TASK_NAME_LEN                 16
/* TickType_t is 'unsigned long' (64-bit) on this port. The app only ever
 * converts ticks to uint32_t milliseconds, so it works with 32-bit ticks too. */
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_64_BITS
#define configIDLE_SHOULD_YIELD                 1

/* ---- Memory: static allocation only ------------------------------------- */
/* Every task, queue and mutex is created with the *Static() API from buffers
 * we declare ourselves, so RAM use is fixed at link time and there is no heap
 * to fragment or exhaust at runtime. (The POSIX port itself still mallocs a
 * small per-thread bookkeeping struct when a task is created, at startup.) */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        0
/* On this port each task runs on a real pthread with its own (8 MB) stack;
 * the FreeRTOS stack buffer only holds the port's thread bookkeeping. */
#define configMINIMAL_STACK_SIZE                ((unsigned short)256)

/* ---- Features ------------------------------------------------------------ */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_TIMERS                        0
#define configUSE_CO_ROUTINES                   0
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configUSE_TRACE_FACILITY                0
#define configGENERATE_RUN_TIME_STATS           0

/* ---- Hooks --------------------------------------------------------------- */
#define configUSE_IDLE_HOOK                     0
/* The tick hook drains the SocketCAN RX FIFO into a FreeRTOS queue from
 * interrupt context. See port/posix/can_socketcan.c. */
#define configUSE_TICK_HOOK                     1
#define configUSE_MALLOC_FAILED_HOOK            0
/* Stack overflow checking is meaningless here (tasks run on pthread stacks);
 * it is worth enabling on the Cortex-M3 port. */
#define configCHECK_FOR_STACK_OVERFLOW          0

/* ---- API inclusion ------------------------------------------------------- */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_vTaskDelete                     0

/* ---- Assertions ---------------------------------------------------------- */
/* A failed configASSERT is a firmware bug: report where and stop. */
void vAssertCalled(const char *file, unsigned long line);
#define configASSERT(x)                         \
    do {                                        \
        if ((x) == 0) {                         \
            vAssertCalled(__FILE__, __LINE__);  \
        }                                       \
    } while (0)

#endif /* FREERTOS_CONFIG_H */
