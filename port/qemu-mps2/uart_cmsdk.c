/*
 * uart_cmsdk.c - CMSDK APB UART, polled output.
 *
 * No RTOS and no interrupts: the bootloader links this file too.
 */
#include "uart_cmsdk.h"

void uart_init_polled(cmsdk_uart_t *uart, uint32_t baud)
{
    uart->CTRL = 0u;
    /* BAUDDIV = PCLK / baud; the UART refuses to run below 16. (QEMU does not
     * model bit timing, but it does check this.) */
    uart->BAUDDIV = MPS2_SYSCLK_HZ / baud;
    uart->CTRL = UART_CTRL_TXEN | UART_CTRL_RXEN;
}

void uart_write_polled(cmsdk_uart_t *uart, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        while ((uart->STATE & UART_STATE_TXFULL) != 0u) {
        }
        uart->DATA = (uint8_t)buf[i];
    }
}

void uart_puts_polled(cmsdk_uart_t *uart, const char *s)
{
    while (*s != '\0') {
        uart_write_polled(uart, s++, 1u);
    }
}

void uart_puthex32_polled(cmsdk_uart_t *uart, uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    char out[10] = { '0', 'x' };
    for (unsigned i = 0; i < 8u; i++) {
        out[2u + i] = hex[(value >> (28u - 4u * i)) & 0xFu];
    }
    uart_write_polled(uart, out, sizeof out);
}
