#include "boot.h"
#include "cpu.h"
#include "early_print.h"
#include "early_selftest.h"
#include "early_uart.h"
#include "intc.h"
#include "kalloc.h"
#include "timer.h"

void kvminit(void);
void kvminithart(void);
void trapinit(void);
void trapinithart(void);

struct mmix_boot_state mmix_boot;

static void mmix_wait(void) __attribute__((noreturn));

static void
mmix_wait(void)
{
  for (;;)
    asm volatile("SWYM 0, 0, 0");
}

void
mmix_start(uint64 startup_cpu_id, uint64 bootinfo_pa)
{
  int bootinfo_status;

  mmix_boot.startup_cpu_id = startup_cpu_id;
  mmix_boot.bootinfo_pa = bootinfo_pa;
  mmix_boot.bootinfo_status =
    mmix_bootinfo_decode(startup_cpu_id, bootinfo_pa, &mmix_boot.info);

  mmix_early_uart_init();
  mmix_early_print_boot(&mmix_boot);
  kinit();
  mmix_early_selftest();
  bootinfo_status = mmix_boot.bootinfo_status;
  kvminit();
  kvminithart();
  trapinit();
  trapinithart();
  if (mmix_intc_init() != MMIX_INTC_OK)
    panic("intc init");
  if (mmix_timer_init() != MMIX_TIMER_OK)
    panic("timer init");
  if (mmix_boot.bootinfo_status != bootinfo_status)
    panic("paging global");
  if (mmix_timer_arm_next() != MMIX_TIMER_OK)
    panic("timer arm");
  if (mmix_intc_set_enabled(MMIX_TIMER_IRQ, 1) != MMIX_INTC_OK)
    panic("timer irq enable");
  intr_on();
  mmix_wait();
}
