#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "early_uart.h"
#include "spinlock.h"
#include "cpu.h"
#include "proc.h"
#include "defs.h"
#include "boot.h"
#include "intc.h"
#include "printk.h"

// QEMU exposes a byte-spaced 16550-compatible UART at UART0_BASE.
enum {
  UART_THR = 0,
  UART_DLL = 0,
  UART_IER = 1,
  UART_DLM = 1,
  UART_FCR = 2,
  UART_IIR = 2,
  UART_LCR = 3,
  UART_LSR = 5,
};

enum {
  UART_FCR_FIFO_ENABLE = 1 << 0,
  UART_FCR_RX_CLEAR = 1 << 1,
  UART_FCR_TX_CLEAR = 1 << 2,
  UART_IER_RX_ENABLE = 1 << 0,
  UART_IER_TX_ENABLE = 1 << 1,
  UART_IIR_NO_INTERRUPT = 1 << 0,
  UART_IIR_REASON_MASK = 0x0f,
  UART_IIR_TX_READY = 0x02,
  UART_IIR_RX_READY = 0x04,
  UART_IIR_RX_TIMEOUT = 0x0c,
  UART_LCR_EIGHT_BITS = 3 << 0,
  UART_LCR_DLAB = 1 << 7,
  UART_LSR_RX_READY = 1 << 0,
  UART_LSR_THR_EMPTY = 1 << 5,
};

static struct spinlock tx_lock;
static int tx_busy;
static int tx_chan;
static int runtime_initialized;
static int runtime_enabled;

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
early_uart_init(void)
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
early_uart_putc(int c)
{
  while ((uart_read(UART_LSR) & UART_LSR_THR_EMPTY) == 0)
    ;

  uart_write(UART_THR, (uint8)c);
}

// Adopt the UART state established by early_uart_init(). Interrupts stay
// disabled until the console, printk lock, and controller are ready.
void
uartinit(void)
{
  if (runtime_initialized || runtime_enabled ||
      intr_get() ||
      mmix_boot.bootinfo_status != MMIX_BOOTINFO_OK ||
      mmix_boot.info.uart_base != UART0_BASE ||
      mmix_boot.info.uart_irq != UART0_IRQ)
    panic("uart init");
  if (uart_read(UART_LCR) != UART_LCR_EIGHT_BITS)
    panic("uart state");

  uart_write(UART_IER, 0);
  initlock(&tx_lock, "uart");
  tx_busy = 0;
  runtime_initialized = 1;
}

static int
uartgetc(void)
{
  if ((uart_read(UART_LSR) & UART_LSR_RX_READY) == 0)
    return -1;
  return uart_read(UART_THR);
}

// Complete the one-way handoff from early polling to the runtime console.
void
uartenable(void)
{
  int c;

  if (!runtime_initialized || runtime_enabled || intr_get())
    panic("uart enable");

  while ((c = uartgetc()) >= 0)
    consoleintr(c);
  (void)uart_read(UART_IIR);
  if (intc_set_enabled(UART0_IRQ, 1) != MMIX_INTC_OK)
    panic("uart irq enable");
  uart_write(UART_IER, UART_IER_RX_ENABLE | UART_IER_TX_ENABLE);
  if (uart_read(UART_IER) != (UART_IER_RX_ENABLE | UART_IER_TX_ENABLE))
    panic("uart irq state");
  runtime_enabled = 1;
}

void
uartwrite(char buf[], int n)
{
  int i = 0;

  if (!runtime_enabled)
    panic("uart write");
  acquire(&tx_lock);
  while (i < n) {
    while (tx_busy)
      sleep(&tx_chan, &tx_lock);
    uart_write(UART_THR, (uint8)buf[i++]);
    tx_busy = 1;
  }
  release(&tx_lock);
}

void
uartputc_sync(int c)
{
  if (!panicking)
    push_off();
  if (panicked)
    for (;;)
      ;

  early_uart_putc(c);

  if (!panicking)
    pop_off();
}

void
uartintr(void)
{
  if (!runtime_enabled)
    panic("uart interrupt state");
  for (;;) {
    uint8 reason = uart_read(UART_IIR) & UART_IIR_REASON_MASK;

    if (reason & UART_IIR_NO_INTERRUPT)
      break;
    if (reason == UART_IIR_TX_READY) {
      acquire(&tx_lock);
      if ((uart_read(UART_LSR) & UART_LSR_THR_EMPTY) == 0) {
        release(&tx_lock);
        panic("uart transmit state");
      }
      tx_busy = 0;
      wakeup(&tx_chan);
      release(&tx_lock);
    } else if (reason == UART_IIR_RX_READY || reason == UART_IIR_RX_TIMEOUT) {
      int c;

      while ((c = uartgetc()) >= 0)
        consoleintr(c);
    } else {
      panic("uart interrupt");
    }
  }
}
