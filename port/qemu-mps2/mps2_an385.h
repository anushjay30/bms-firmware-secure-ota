/*
 * mps2_an385.h - The handful of Cortex-M3 core and MPS2-AN385 peripheral
 * registers this port uses.
 *
 * Written out by hand instead of pulling in CMSIS so every register access is
 * visible and explained. Addresses: ARMv7-M Architecture Reference Manual
 * (System Control Space at 0xE000E000) and the AN385 / QEMU hw/arm/mps2.c
 * memory map for the CMSDK peripherals.
 */
#ifndef MPS2_AN385_H
#define MPS2_AN385_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* ---- Clocks ------------------------------------------------------------- */
#define MPS2_SYSCLK_HZ        25000000u  /* CPU + APB peripheral clock */

/* ---- System Control Block ------------------------------------------------ */
#define SCB_ICSR              REG32(0xE000ED04u) /* interrupt control/state      */
#define SCB_VTOR              REG32(0xE000ED08u) /* vector table offset          */
#define SCB_AIRCR             REG32(0xE000ED0Cu) /* app interrupt/reset control  */
#define SCB_CCR               REG32(0xE000ED14u) /* configuration and control    */
#define SCB_SHCSR             REG32(0xE000ED24u) /* system handler control/state */
#define SCB_CFSR              REG32(0xE000ED28u) /* configurable fault status    */
#define SCB_HFSR              REG32(0xE000ED2Cu) /* HardFault status             */
#define SCB_MMFAR             REG32(0xE000ED34u) /* MemManage fault address      */
#define SCB_BFAR              REG32(0xE000ED38u) /* BusFault address             */

#define SCB_ICSR_VECTACTIVE_MASK 0x1FFu
#define SCB_ICSR_PENDSTCLR       (1u << 25) /* clear a pending SysTick exception */
#define SCB_ICSR_PENDSVCLR       (1u << 27) /* clear a pending PendSV exception  */
#define SCB_AIRCR_VECTKEY        (0x05FAu << 16) /* writes are ignored without it */
#define SCB_AIRCR_SYSRESETREQ    (1u << 2)
#define SCB_CCR_DIV_0_TRP        (1u << 4)

#define SCB_HFSR_FORCED          (1u << 30) /* escalated from a configurable fault */
#define SCB_HFSR_VECTTBL         (1u << 1)  /* bus fault while reading a vector     */
#define SCB_CFSR_MMARVALID       (1u << 7)
#define SCB_CFSR_BFARVALID       (1u << 15)

/* ---- SysTick ------------------------------------------------------------------ */
#define SYST_CSR              REG32(0xE000E010u) /* control and status: [0] enable [1] interrupt */
#define SYST_RVR              REG32(0xE000E014u) /* reload value                              */
#define SYST_CVR              REG32(0xE000E018u) /* current value (any write clears it)       */
#define SYST_CSR_ENABLE       (1u << 0)
#define SYST_CSR_CLKSOURCE    (1u << 2)          /* count the processor clock                 */

/* ---- NVIC ------------------------------------------------------------------ */
#define NVIC_ISER0            REG32(0xE000E100u) /* set-enable, IRQ 0..31   */
#define NVIC_ICER0            REG32(0xE000E180u) /* clear-enable            */
#define NVIC_ISPR0            REG32(0xE000E200u) /* set-pending             */
#define NVIC_ICPR0            REG32(0xE000E280u) /* clear-pending           */
#define NVIC_IPR              ((volatile uint8_t *)0xE000E400u) /* 1 byte per IRQ */

/* Interrupt numbers (IRQn, i.e. vector index - 16) as wired by QEMU's AN385. */
typedef enum {
    UART0_RX_IRQn  = 0,
    UART0_TX_IRQn  = 1,
    UART1_RX_IRQn  = 2,
    UART1_TX_IRQn  = 3,
    UART2_RX_IRQn  = 4,
    UART2_TX_IRQn  = 5,
    TIMER0_IRQn    = 8,
    TIMER1_IRQn    = 9,
    DUALTIMER_IRQn = 10,
    UART_OVF_IRQn  = 12, /* overrun of UART0..2, ORed together */
    MPS2_IRQ_COUNT = 32
} mps2_irqn_t;

static inline void nvic_enable_irq(mps2_irqn_t irq)      { NVIC_ISER0 = 1u << (unsigned)irq; }
static inline void nvic_disable_irq(mps2_irqn_t irq)     { NVIC_ICER0 = 1u << (unsigned)irq; }
static inline void nvic_set_pending(mps2_irqn_t irq)     { NVIC_ISPR0 = 1u << (unsigned)irq; }
static inline void nvic_clear_pending(mps2_irqn_t irq)   { NVIC_ICPR0 = 1u << (unsigned)irq; }

/* Priorities live in the top bits of each 8-bit IPR field; lower value =
 * more urgent. Callers pass the already-shifted value. */
static inline void nvic_set_priority(mps2_irqn_t irq, uint8_t prio) { NVIC_IPR[irq] = prio; }

/* ---- CMSDK APB UART ------------------------------------------------------ */
typedef struct {
    volatile uint32_t DATA;      /* 0x00 RX/TX byte                                  */
    volatile uint32_t STATE;     /* 0x04 [0] TX full [1] RX full [2] TX ovr [3] RX ovr */
    volatile uint32_t CTRL;      /* 0x08 enables, see below                          */
    volatile uint32_t INTSTATUS; /* 0x0C read: pending IRQs; write 1: clear (INTCLEAR) */
    volatile uint32_t BAUDDIV;   /* 0x10 PCLK / baud, must be >= 16                    */
} cmsdk_uart_t;

#define CMSDK_UART0           ((cmsdk_uart_t *)0x40004000u)
#define CMSDK_UART1           ((cmsdk_uart_t *)0x40005000u)

#define UART_STATE_TXFULL     (1u << 0)
#define UART_STATE_RXFULL     (1u << 1)
#define UART_STATE_TXOVR      (1u << 2)
#define UART_STATE_RXOVR      (1u << 3)

#define UART_CTRL_TXEN        (1u << 0)
#define UART_CTRL_RXEN        (1u << 1)
#define UART_CTRL_TXIE        (1u << 2)
#define UART_CTRL_RXIE        (1u << 3)

#define UART_INT_TX           (1u << 0)
#define UART_INT_RX           (1u << 1)

#endif /* MPS2_AN385_H */
