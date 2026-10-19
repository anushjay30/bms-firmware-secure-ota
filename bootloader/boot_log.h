/*
 * boot_log.h - Polled log output on UART0 for the bootloader.
 *
 * A tiny formatter instead of the C library's printf: it keeps the
 * bootloader small and free of the heap that newlib's printf can pull in.
 */
#ifndef BOOT_LOG_H
#define BOOT_LOG_H

#include <stdint.h>

void boot_log_init(void);

/*
 * printf-like, with only what the bootloader needs. Every numeric argument
 * must be passed as uint32_t (cast at the call site):
 *   %s  string        %c  character
 *   %u  decimal       %x  hex, 8 digits with 0x prefix
 *   %v  image version (major << 16 | minor << 8 | patch) as "1.2.3"
 */
void boot_printf(const char *fmt, ...);

#endif /* BOOT_LOG_H */
