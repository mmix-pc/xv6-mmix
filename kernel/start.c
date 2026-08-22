#include "boot.h"
#include "diagnostic.h"
#include "early_uart.h"

void main(void) __attribute__((noreturn));

struct mmix_boot_state mmix_boot;

void
start(uint64 startup_cpu_id, uint64 bootinfo_pa)
{
  mmix_boot.startup_cpu_id = startup_cpu_id;
  mmix_boot.bootinfo_pa = bootinfo_pa;
  mmix_boot.bootinfo_status =
    bootinfo_decode(startup_cpu_id, bootinfo_pa, &mmix_boot.info);

  early_uart_init();
  diagnostic_boot(&mmix_boot);
  main();
}
