/*
 * bms_port_config.h - Per-port sizing used by app/tasks (Cortex-M3).
 *
 * Stack depths are in 32-bit words and are real RAM. Method: start generous
 * (512 words each), drive one boot through every major code path
 * (python -m bms_sim.exercise: faults, resets, watchdog, bad CRC, sensor
 * loss, debug report), read the high-water marks it logs, then size each
 * stack to about twice the measured peak, rounded up to 64 words.
 *
 *   task        peak used   size     (arm-none-eabi-gcc 14.2, -Os;
 *   can_rx        161        384      GCC 16 measured within 5 words)
 *   monitor       200        448
 *   fault         179        384
 *   contactor     172        384
 *   can_tx         83        256   (its error-logging path never ran in the
 *                                   exercise; bms_log() alone needs ~150 words)
 *   logger        226        448
 *   supervisor    169        384
 *   ota           684       1408   (a whole update: 44 KB received, SHA-512 +
 *                                   Ed25519 verification - Monocypher's
 *                                   field/point arithmetic is the bulk of it)
 *   idle           19        128   (configMINIMAL_STACK_SIZE)
 *
 * Whatever formats a log line dominates: bms_log() keeps a 132-byte record on
 * the caller's stack and vsnprintf() needs a few hundred bytes more. Stack
 * overflow checking (method 2) stays on as the safety net. Re-measure after
 * any change that adds logging or deeper call chains.
 */
#ifndef BMS_PORT_CONFIG_H
#define BMS_PORT_CONFIG_H

#define BMS_STACK_WORDS_DEFAULT       384u
#define BMS_STACK_WORDS_CAN_RX        384u
#define BMS_STACK_WORDS_MONITOR       448u
#define BMS_STACK_WORDS_FAULT         384u
#define BMS_STACK_WORDS_CONTACTOR     384u
#define BMS_STACK_WORDS_CAN_TX        256u
#define BMS_STACK_WORDS_LOGGER        448u
#define BMS_STACK_WORDS_SUPERVISOR    384u
/* Signature verification (Monocypher) is the deepest path: see the table. */
#define BMS_STACK_WORDS_OTA           1408u

#define BMS_PORT_HAS_REAL_TASK_STACKS 1

/* This port has a bootloader, A/B slots and flash: updates over CAN. */
#define BMS_PORT_HAS_OTA              1

/* UART interrupt priority: logical level 6, below (= less urgent than)
 * configMAX_SYSCALL_INTERRUPT_PRIORITY (5), so the ISRs may use FromISR APIs. */
#define BMS_IRQ_PRIO_UART             (6u << (8u - configPRIO_BITS))

#define BMS_UART_BAUD                 115200u

#endif /* BMS_PORT_CONFIG_H */
