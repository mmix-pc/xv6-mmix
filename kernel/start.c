#include "boot.h"
#include "early_print.h"
#include "early_uart.h"

struct mmix_boot_state mmix_boot;

static void
mmix_wait(void) __attribute__((noreturn));

static void
mmix_wait(void)
{
  for (;;)
    asm volatile("SWYM 0, 0, 0");
}

void
mmix_start(uint64 startup_cpu_id, uint64 bootinfo_pa)
{
  mmix_boot.startup_cpu_id = startup_cpu_id;
  mmix_boot.bootinfo_pa = bootinfo_pa;
  mmix_boot.bootinfo_status =
      mmix_bootinfo_decode(startup_cpu_id, bootinfo_pa, &mmix_boot.info);

  mmix_early_uart_init();
  mmix_early_print_boot(&mmix_boot);
  mmix_wait();
}
