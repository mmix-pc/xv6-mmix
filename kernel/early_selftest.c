#include "mmix.h"
#include "spinlock.h"
#include "cpu.h"
#include "defs.h"
#include "early_selftest.h"

// Temporary boot-time tests for low-level kernel foundations. Extend this
// function as new early subsystems need validation, and remove individual
// checks once normal kernel operation provides equivalent coverage.
void
mmix_early_selftest(void)
{
  struct spinlock lock;
  struct cpu *cpu = &cpus[BOOT_CPU_ID];

  if (intr_get() || cpuid() != BOOT_CPU_ID || mycpu() != cpu ||
      cpu->noff != 0 || cpu->intena != 0)
    panic("cpu state");

  push_off();
  push_off();
  if (intr_get() || cpu->noff != 2 || cpu->intena != 0)
    panic("push_off");
  pop_off();
  pop_off();
  if (intr_get() || cpu->noff != 0 || cpu->intena != 0)
    panic("pop_off");

  initlock(&lock, "boot");
  acquire(&lock);
  if (!holding(&lock) || lock.cpu != cpu)
    panic("acquire");
  release(&lock);
  if (lock.locked != 0 || lock.cpu != 0 || cpu->noff != 0 || intr_get())
    panic("release");
}
