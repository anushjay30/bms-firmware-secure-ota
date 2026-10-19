/*
 * bms_port_config.h - Per-port sizing used by app/tasks.
 *
 * Stack depths are in StackType_t words. On the POSIX port each task really
 * runs on its own pthread stack, so the FreeRTOS stack buffer only holds the
 * port's thread bookkeeping and one small default fits all tasks. Compare
 * port/qemu-mps2/bms_port_config.h, where every byte is real RAM.
 */
#ifndef BMS_PORT_CONFIG_H
#define BMS_PORT_CONFIG_H

#define BMS_STACK_WORDS_DEFAULT       256u

/* uxTaskGetStackHighWaterMark() would only measure the unused bookkeeping
 * buffer here, so the stack report says "not meaningful" instead. */
#define BMS_PORT_HAS_REAL_TASK_STACKS 0

/* No bootloader and no flash on the host: firmware updates are a Cortex-M3
 * (QEMU) feature. */
#define BMS_PORT_HAS_OTA              0

#endif /* BMS_PORT_CONFIG_H */
