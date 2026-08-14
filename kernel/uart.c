#include "types.h"
#include "memlayout.h"
#include "early_uart.h"

// QEMU exposes a byte-spaced 16550-compatible UART at UART0_BASE.
enum {
  UART_THR = 0,
  UART_DLL = 0,
  UART_IER = 1,
  UART_DLM = 1,
  UART_FCR = 2,
  UART_LCR = 3,
  UART_LSR = 5,
};

enum {
  UART_FCR_FIFO_ENABLE = 1 << 0,
  UART_FCR_RX_CLEAR = 1 << 1,
  UART_FCR_TX_CLEAR = 1 << 2,
  UART_LCR_EIGHT_BITS = 3 << 0,
  UART_LCR_DLAB = 1 << 7,
  UART_LSR_THR_EMPTY = 1 << 5,
};

static __attribute__((always_inline)) inline uint8
uart_read(uint64 offset)
{
  return *(volatile uint8 *)(UART0_BASE + offset);
}

static __attribute__((always_inline)) inline void
uart_write(uint64 offset, uint8 value)
{
  *(volatile uint8 *)(UART0_BASE + offset) = value;
}

void
mmix_early_uart_init(void)
{
  // Keep all UART interrupt sources disabled during early boot.
  uart_write(UART_IER, 0);

  // QEMU's 115200 baud base and divisor 1 select 115200 baud.
  uart_write(UART_LCR, UART_LCR_DLAB);
  uart_write(UART_DLL, 1);
  uart_write(UART_DLM, 0);

  // Select 8 data bits, one stop bit, and no parity, then reset both FIFOs.
  uart_write(UART_LCR, UART_LCR_EIGHT_BITS);
  uart_write(UART_FCR, UART_FCR_FIFO_ENABLE | UART_FCR_RX_CLEAR |
                           UART_FCR_TX_CLEAR);
  uart_write(UART_IER, 0);
}

void
mmix_early_uart_putc(int c)
{
  while ((uart_read(UART_LSR) & UART_LSR_THR_EMPTY) == 0)
    ;

  uart_write(UART_THR, (uint8)c);
}
