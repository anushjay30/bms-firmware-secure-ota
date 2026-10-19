/*
 * boot_log.c - Polled log output on UART0 for the bootloader.
 */
#include "boot_log.h"

#include <stdarg.h>
#include <stddef.h>

#include "uart_cmsdk.h"

#define BOOT_UART      CMSDK_UART0
#define BOOT_UART_BAUD 115200u

void boot_log_init(void)
{
    uart_init_polled(BOOT_UART, BOOT_UART_BAUD);
}

static void put_char(char c)
{
    uart_write_polled(BOOT_UART, &c, 1u);
}

static void put_dec(uint32_t v)
{
    char buf[10];
    size_t i = sizeof buf;
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    uart_write_polled(BOOT_UART, &buf[i], sizeof buf - i);
}

void boot_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%' || p[1] == '\0') {
            put_char(*p);
            continue;
        }
        switch (*++p) {
        case 's':
            uart_puts_polled(BOOT_UART, va_arg(ap, const char *));
            break;
        case 'c':
            put_char((char)va_arg(ap, int));
            break;
        case 'u':
            put_dec(va_arg(ap, uint32_t));
            break;
        case 'x':
            uart_puthex32_polled(BOOT_UART, va_arg(ap, uint32_t));
            break;
        case 'v': {
            const uint32_t v = va_arg(ap, uint32_t);
            put_dec(v >> 16);
            put_char('.');
            put_dec((v >> 8) & 0xFFu);
            put_char('.');
            put_dec(v & 0xFFu);
            break;
        }
        default:
            put_char(*p); /* "%%" and anything unknown print literally */
            break;
        }
    }
    va_end(ap);
}
