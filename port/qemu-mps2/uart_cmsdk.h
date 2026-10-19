/*
 * uart_cmsdk.h - ARM CMSDK APB UART, polled mode.
 *
 * Used for UART0, the log console, by code that must not depend on
 * interrupts or the RTOS: the bootloader (bare metal, no interrupts at all),
 * the application before the scheduler starts and in its logger task, and
 * the fault handlers, which run when the RTOS itself may be broken.
 * The interrupt-driven stream on UART1 (the CAN link) is in uart_stream.c.
 */
#ifndef UART_CMSDK_H
#define UART_CMSDK_H

#include <stddef.h>
#include <stdint.h>

#include "mps2_an385.h"

void uart_init_polled(cmsdk_uart_t *uart, uint32_t baud);
void uart_write_polled(cmsdk_uart_t *uart, const char *buf, size_t len);
void uart_puts_polled(cmsdk_uart_t *uart, const char *s);
void uart_puthex32_polled(cmsdk_uart_t *uart, uint32_t value); /* "0x1234ABCD" */

#endif /* UART_CMSDK_H */
