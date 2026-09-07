#include "types.h"
#include "param.h"
#include "memlayout.h"
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

#define EARLY_UART_ALIAS 0x8001000010000000UL

static struct spinlock tx_lock;
static int tx_busy;
static int tx_chan;
static int runtime_initialized;
static int runtime_enabled;
static uint32 output_owner;
static uint32 panic_owner;

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

static __attribute__((always_inline)) inline uint8
early_uart_read(uint64 offset)
{
  return *(volatile uint8 *)(EARLY_UART_ALIAS + offset);
}

static __attribute__((always_inline)) inline void
early_uart_write(uint64 offset, uint8 value)
{
  *(volatile uint8 *)(EARLY_UART_ALIAS + offset) = value;
}

void
early_uart_init(void)
{
  // Keep all UART interrupt sources disabled during early boot.
  early_uart_write(UART_IER, 0);

  // QEMU's 115200 baud base and divisor 1 select 115200 baud.
  early_uart_write(UART_LCR, UART_LCR_DLAB);
  early_uart_write(UART_DLL, 1);
  early_uart_write(UART_DLM, 0);

  // Select 8 data bits, one stop bit, and no parity, then reset both FIFOs.
  early_uart_write(UART_LCR, UART_LCR_EIGHT_BITS);
  early_uart_write(UART_FCR, UART_FCR_FIFO_ENABLE | UART_FCR_RX_CLEAR |
                                 UART_FCR_TX_CLEAR);
  early_uart_write(UART_IER, 0);
}

static void
early_uart_putc(int c)
{
  while ((early_uart_read(UART_LSR) & UART_LSR_THR_EMPTY) == 0)
    ;

  early_uart_write(UART_THR, (uint8)c);
}

// Serialize every normal THR writer, including synchronous diagnostics and
// echo, without making the panic path depend on an xv6 lock.
static void
uart_output_enter(void)
{
  uint32 current = (uint32)cpuid() + 1;
  uint32 expected;

  for (;;) {
    if (__atomic_load_n(&panic_owner, __ATOMIC_ACQUIRE) != 0)
      for (;;)
        ;
    expected = 0;
    if (__atomic_compare_exchange_n(&output_owner, &expected, current, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      if (__atomic_load_n(&panic_owner, __ATOMIC_ACQUIRE) == 0)
        return;
      __atomic_store_n(&output_owner, 0, __ATOMIC_RELEASE);
    }
  }
}

static void
uart_output_leave(void)
{
  __atomic_store_n(&output_owner, 0, __ATOMIC_RELEASE);
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
  __atomic_store_n(&tx_busy, 0, __ATOMIC_RELAXED);
  runtime_initialized = 1;
}

static int
uartgetc(void)
{
  if ((uart_read(UART_LSR) & UART_LSR_RX_READY) == 0)
    return -1;
  return uart_read(UART_THR);
}

static void __attribute__((noreturn))
uart_fail(char *message)
{
  uint32 enabled = 0;
  uint32 owner = ~0U;
  int enabled_status = intc_enabled(&enabled);
  int owner_status = intc_shared_owner(UART0_IRQ, &owner);

  printk("uart failure: cpu=%d owner=%u/%d enabled=0x%x/%d "
         "ier=0x%x iir=0x%x lsr=0x%x tx=%d output-owner=%u\n",
         cpuid(), owner, owner_status, enabled, enabled_status,
         (unsigned int)uart_read(UART_IER),
         (unsigned int)uart_read(UART_IIR),
         (unsigned int)uart_read(UART_LSR),
         __atomic_load_n(&tx_busy, __ATOMIC_RELAXED),
         __atomic_load_n(&output_owner, __ATOMIC_RELAXED));
  panic(message);
}

// Complete the one-way handoff from early polling to the runtime console.
void
uartenable(void)
{
  int c;
  uint32 enabled;
  uint32 owner;

  if (!runtime_initialized || runtime_enabled || intr_get())
    panic("uart enable");

  if (intc_shared_owner(UART0_IRQ, &owner) != MMIX_INTC_OK ||
      owner != (uint32)cpuid() ||
      intc_enabled(&enabled) != MMIX_INTC_OK ||
      (enabled & (1U << UART0_IRQ)) == 0)
    uart_fail("uart affinity");

  while ((c = uartgetc()) >= 0)
    consoleintr(c);
  (void)uart_read(UART_IIR);
  uart_write(UART_IER, UART_IER_RX_ENABLE | UART_IER_TX_ENABLE);
  if (uart_read(UART_IER) != (UART_IER_RX_ENABLE | UART_IER_TX_ENABLE))
    uart_fail("uart irq state");
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
    while (__atomic_load_n(&tx_busy, __ATOMIC_RELAXED))
      sleep(&tx_chan, &tx_lock);
    uart_output_enter();
    uart_write(UART_THR, (uint8)buf[i++]);
    uart_output_leave();
    __atomic_store_n(&tx_busy, 1, __ATOMIC_RELAXED);
  }
  release(&tx_lock);
}

void
uartputc_sync(int c)
{
  uint32 owner = __atomic_load_n(&panic_owner, __ATOMIC_ACQUIRE);

  if (owner != 0) {
    if (owner != (uint32)cpuid() + 1 ||
        __atomic_load_n(&panicked, __ATOMIC_ACQUIRE))
      for (;;)
        ;
    early_uart_putc(c);
    return;
  }

  push_off();
  if (__atomic_load_n(&panicked, __ATOMIC_ACQUIRE))
    for (;;)
      ;
  uart_output_enter();
  early_uart_putc(c);
  uart_output_leave();
  pop_off();
}

// Elect one panic writer, prevent new normal THR writes, and drain any writer
// that entered before the election. This cannot deadlock on an xv6 lock held
// by the panicking or an interrupted CPU.
void
uartpanic(void)
{
  uint32 owner = (uint32)cpuid() + 1;
  uint32 expected = 0;

  if (!__atomic_compare_exchange_n(&panic_owner, &expected, owner, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
      expected != owner)
    for (;;)
      ;
  early_uart_write(UART_IER, 0);
  while (__atomic_load_n(&output_owner, __ATOMIC_ACQUIRE) != 0)
    ;
  while ((early_uart_read(UART_LSR) & UART_LSR_THR_EMPTY) == 0)
    ;
}

void
uartintr(void)
{
  if (!runtime_enabled)
    uart_fail("uart interrupt state");
  for (;;) {
    uint8 reason = uart_read(UART_IIR) & UART_IIR_REASON_MASK;

    if (reason & UART_IIR_NO_INTERRUPT)
      break;
    if (reason == UART_IIR_TX_READY) {
      acquire(&tx_lock);
      if ((uart_read(UART_LSR) & UART_LSR_THR_EMPTY) == 0) {
        release(&tx_lock);
        uart_fail("uart transmit state");
      }
      __atomic_store_n(&tx_busy, 0, __ATOMIC_RELAXED);
      wakeup(&tx_chan);
      release(&tx_lock);
    } else if (reason == UART_IIR_RX_READY || reason == UART_IIR_RX_TIMEOUT) {
      int c;

      while ((c = uartgetc()) >= 0)
        consoleintr(c);
    } else {
      uart_fail("uart interrupt");
    }
  }
}
