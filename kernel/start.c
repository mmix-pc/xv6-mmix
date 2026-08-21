#include "boot.h"
#include "kcontext.h"
#include "cpu.h"
#include "diagnostic.h"
#include "early_selftest.h"
#include "early_uart.h"
#include "printk.h"
#include "intc.h"
#include "kalloc.h"
#include "memlayout.h"
#include "timer.h"

void kvminit(void);
void kvminithart(void);
void trapinit(void);
void trapinithart(void);
void procinit(void);
void scheduler(void) __attribute__((noreturn));
void swtch(struct context *, struct context *);
void consoleinit(void);
void uartenable(void);

struct mmix_boot_state mmix_boot;

void
mmix_start(uint64 startup_cpu_id, uint64 bootinfo_pa)
{
  struct context boot_context;
  int bootinfo_status;

  mmix_boot.startup_cpu_id = startup_cpu_id;
  mmix_boot.bootinfo_pa = bootinfo_pa;
  mmix_boot.bootinfo_status =
    mmix_bootinfo_decode(startup_cpu_id, bootinfo_pa, &mmix_boot.info);

  mmix_early_uart_init();
  mmix_diagnostic_boot(&mmix_boot);
  kinit();
  mmix_early_selftest();
  bootinfo_status = mmix_boot.bootinfo_status;
  kvminit();
  kvminithart();
  mmix_kcontext_init();
  procinit();
  mmix_kcontext_prepare(&cpus[BOOT_CPU_ID].context,
                        MMIX_CONTEXT_SCHEDULER_SLOT, scheduler);
  trapinit();
  trapinithart();
  if (mmix_intc_init() != MMIX_INTC_OK)
    panic("intc init");
  consoleinit();
  printkinit();
  if (mmix_timer_init() != MMIX_TIMER_OK)
    panic("timer init");
  if (mmix_boot.bootinfo_status != bootinfo_status)
    panic("paging global");
  if (mmix_timer_arm_next() != MMIX_TIMER_OK)
    panic("timer arm");
  if (mmix_intc_set_enabled(MMIX_TIMER_IRQ, 1) != MMIX_INTC_OK)
    panic("timer irq enable");
  uartenable();
  intr_off();
  swtch(&boot_context, &cpus[BOOT_CPU_ID].context);
  panic("scheduler returned");
}
