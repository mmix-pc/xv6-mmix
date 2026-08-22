#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmix.h"
#include "defs.h"
#include "intc.h"
#include "kcontext.h"
#include "timer.h"

void main(void) __attribute__((noreturn));

// start() jumps here after establishing the minimal MMIX boot state.
void
main(void)
{
  kinit();            // physical page allocator
  kvminit();          // create kernel page table
  kvminithart();      // turn on paging
  kcontext_init();
  procinit();         // process table
  trapinit();         // trap vectors
  trapinithart();     // install kernel trap vector
  if (intc_init() != MMIX_INTC_OK)
    panic("intc init");
  consoleinit();
  printkinit();
  binit();            // buffer cache
  iinit();            // inode table
  fileinit();         // file table
  virtio_disk_init(); // emulated hard disk
  if (timer_init() != MMIX_TIMER_OK)
    panic("timer init");
  if (timer_arm_next() != MMIX_TIMER_OK)
    panic("timer arm");
  if (intc_set_enabled(MMIX_TIMER_IRQ, 1) != MMIX_INTC_OK)
    panic("timer irq enable");
  uartenable();
  userinit();         // first user process

  scheduler();
}
