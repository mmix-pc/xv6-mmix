#include "boot.h"
#include "early_print.h"
#include "early_selftest.h"
#include "early_uart.h"
#include "intc.h"
#include "kalloc.h"

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
  if (mmix_boot.bootinfo_status != bootinfo_status)
    panic("paging global");
  mmix_wait();
}
