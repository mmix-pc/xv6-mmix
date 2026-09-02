#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmix.h"
#include "defs.h"
#include "boot.h"
#include "cpu.h"
#include "diagnostic.h"
#include "intc.h"
#include "ipi.h"
#include "kalloc.h"
#include "kcontext.h"
#include "timer.h"
#include "vm.h"

void main(void) __attribute__((noreturn));

// start() jumps here after establishing the minimal MMIX boot state.
void
main(void)
{
  struct kalloc_stats allocator_stats;
  uint32 timer_irq_number;

  if (mmix_boot.bootinfo_status != MMIX_BOOTINFO_OK)
    panic("bootinfo");
  kinit();            // physical page allocator
  kvminit();          // create kernel page table
  kvminithart();      // turn on paging
  diagnostic_paging(kernel_pagetable->rv);
  kinit_reclaimed();  // add mapped Pool, Data, and Stack backing
  kinit_high();       // add optional mapped High RAM
  kcontext_init();
  kalloc_get_stats(&allocator_stats);
  diagnostic_allocator(&allocator_stats);
  procinit();         // process table
  trapinit();         // trap vectors
  trapinithart();     // install kernel trap vector
  trapenablehart();   // enable CPU 0 program traps
  if (intc_validate() != MMIX_INTC_OK || timer_validate() != MMIX_TIMER_OK ||
      ipi_validate() != MMIX_IPI_OK)
    panic("interrupt platform");
  if (intc_init() != MMIX_INTC_OK || timer_init() != MMIX_TIMER_OK ||
      ipi_init() != MMIX_IPI_OK)
    panic("interrupt context");
  consoleinit();
  printkinit();
  binit();            // buffer cache
  iinit();            // inode table
  fileinit();         // file table
  virtio_disk_init(); // emulated hard disk
  if (intc_publish_affinity() != MMIX_INTC_OK)
    panic("interrupt affinity publish");
  if (boot_publish_global_ready() < 0)
    panic("global publish");
  if (boot_wait_for_online() < 0)
    panic("CPU online");

  // CPU 0 retains the single-core service path after every CPU is online.
  if (timer_arm_next() != MMIX_TIMER_OK)
    panic("timer arm");
  if (timer_irq(&timer_irq_number) != MMIX_TIMER_OK ||
      intc_enable_runtime(timer_irq_number) != MMIX_INTC_OK)
    panic("interrupt affinity");
  uartenable();
  intr_on();
  while (timer_ticks() == 0)
    cpu_idle();
  if (boot_publish_interrupt_ready() < 0 ||
      boot_wait_for_interrupt_ready() < 0)
    panic("interrupt ready");
  intr_off();
  userinit();         // first user process
  if (boot_release_schedulers() < 0)
    panic("scheduler release");

  scheduler();
}
