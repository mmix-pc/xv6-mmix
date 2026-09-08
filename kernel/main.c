#include "types.h"
#include "mmix.h"
#include "defs.h"
#include "boot.h"
#include "diagnostic.h"
#include "platform.h"
#include "physmem.h"
#include "kalloc.h"
#include "kcontext.h"
#include "cpu.h"
#include "vm.h"
#include "intc.h"
#include "ipi.h"
#include "timer.h"

void main(void) __attribute__((noreturn));
void secondary_main(void) __attribute__((noreturn));

static uint64 memory_ready;

static void
interrupt_ready(void)
{
  uint32 irq;

  trapinithart();
  if (intc_init() != MMIX_INTC_OK || timer_init() != MMIX_TIMER_OK ||
      ipi_init() != MMIX_IPI_OK || timer_irq(&irq) != MMIX_TIMER_OK ||
      intc_enable_runtime(irq) != MMIX_INTC_OK ||
      timer_arm_next() != MMIX_TIMER_OK)
    panic("local interrupts");
  trapenablehart();
  intr_on();
  while (timer_ticks() == 0)
    cpu_idle();
  if (boot_publish_interrupt_ready() < 0)
    panic("interrupt readiness");
}

static void
kernel_ready(void)
{
  struct kalloc_stats stats;

  interrupt_ready();
  if (boot_wait_for_interrupt_ready() < 0)
    panic("CPU interrupt readiness");
  intr_off();
  if (boot_reclaim_stacks() != PHYSMEM_OK)
    panic("bootstrap release");
  kalloc_get_stats(&stats);
  if (stats.managed_pages != physmem_managed_pages() ||
      stats.reserved_pages != physmem_reserved_pages())
    panic("memory ownership");
  diagnostic_paging(mmix_rv_read());
  diagnostic_allocator(&stats);
  printk("interrupt-service ready: cpus=%d\n", (int)platform_cpu_count());
  if (boot_release_schedulers() < 0)
    panic("scheduler release");
  scheduler();
}

void
secondary_main(void)
{
  while (__atomic_load_n(&memory_ready, __ATOMIC_ACQUIRE) == 0) {
    if (__atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) ==
        MMIX_STARTUP_FAILED)
      panic("shared initialization");
    cpu_idle();
  }
  kvminithart();
  cpu_secondary_enter(interrupt_ready);
}

// start() jumps here after publishing the immutable platform description.
void
main(void)
{
  if (boot_wait_for_online() < 0)
    panic("CPU online");
  diagnostic_platform_checkpoint();
  printkinit();

  kinit();
  if (boot_reclaim_fdt() != PHYSMEM_OK)
    panic("FDT release");
  kvminit();
  kvminithart();
  kcontext_init();
  procinit();
  trapinit();
  if (intc_init() != MMIX_INTC_OK ||
      intc_publish_affinity() != MMIX_INTC_OK ||
      timer_validate() != MMIX_TIMER_OK || ipi_validate() != MMIX_IPI_OK)
    panic("interrupt platform");
  __atomic_store_n(&memory_ready, 1, __ATOMIC_RELEASE);
  cpu_context_enter(kernel_ready);
}
