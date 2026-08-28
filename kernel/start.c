#include "boot.h"
#include "memlayout.h"
#include "cpu.h"
#include "diagnostic.h"
#include "early_uart.h"

void main(void) __attribute__((noreturn));

struct mmix_boot_state mmix_boot;
struct mmix_boot_handoff mmix_boot_handoffs[MMIX_MAX_CPUS];

// FIXME: Replace this containment loop with the guest-owned SMP startup
// barrier when global initialization can be published to secondary CPUs.
static void secondary_wait(void) __attribute__((noreturn));

static void
secondary_wait(void)
{
  for (;;)
    asm volatile("SWYM 0, 0, 0" ::: "memory");
}

void
start(uint64 startup_cpu_id, uint64 bootinfo_pa)
{
  struct mmix_boot_handoff *handoff;

  // _entry performs this check before selecting a stack. Retain a C-side
  // boundary check so future callers cannot index the handoff array unsafely.
  if (startup_cpu_id >= MMIX_MAX_CPUS)
    secondary_wait();

  handoff = &mmix_boot_handoffs[startup_cpu_id];
  handoff->startup_cpu_id = startup_cpu_id;
  handoff->bootinfo_pa = bootinfo_pa;
  handoff->software_stack_base = BOOT_STACK_BASE(startup_cpu_id);
  handoff->software_stack_top = BOOT_STACK_TOP(startup_cpu_id);
  handoff->register_stack_base = BOOT_REGISTER_STACK_BASE(startup_cpu_id);
  handoff->register_stack_limit = BOOT_REGISTER_STACK_LIMIT(startup_cpu_id);

  if ((uint64)cpuid() != startup_cpu_id ||
      mycpu() != &cpus[startup_cpu_id])
    secondary_wait();

  if (startup_cpu_id != BOOT_CPU_ID)
    secondary_wait();

  mmix_boot.startup_cpu_id = startup_cpu_id;
  mmix_boot.bootinfo_pa = bootinfo_pa;
  mmix_boot.bootinfo_status =
    bootinfo_decode(startup_cpu_id, bootinfo_pa, &mmix_boot.info);

  early_uart_init();
  diagnostic_boot(&mmix_boot);
  main();
}
