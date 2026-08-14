#include "mmix.h"
#include "spinlock.h"
#include "cpu.h"
#include "defs.h"
#include "early_selftest.h"
#include "kalloc.h"

extern char kernel_end[];

static void
allocator_selftest(void)
{
  void *pages[3];
  void *reused[3];
  uint64 first = KALLOC_START((uint64)kernel_end);
  uint64 expected = (KALLOC_LIMIT - first) / PGSIZE;

  if (first < KERNEL_LOAD || first >= KALLOC_LIMIT || expected < 3 ||
      (first & (PGSIZE - 1)) != 0 || kalloc_free_pages() != expected)
    panic("kalloc range");

  if (!kalloc_page_is_managed((void *)first) ||
      !kalloc_page_is_managed((void *)(KALLOC_LIMIT - PGSIZE)) ||
      kalloc_page_is_managed((void *)(first - PGSIZE)) ||
      kalloc_page_is_managed((void *)KALLOC_LIMIT) ||
      kalloc_page_is_managed((void *)REGISTER_STACK_BASE) ||
      kalloc_page_is_managed((void *)KERNEL_ROOT_BASE) ||
      kalloc_page_is_managed((void *)KERNEL_LOAD) ||
      kalloc_page_is_managed((void *)POOL_PHYS_BASE))
    panic("kalloc bounds");

  for (int i = 0; i < 3; i++) {
    pages[i] = kalloc();
    if (pages[i] == 0 || !kalloc_page_is_managed(pages[i]))
      panic("kalloc alloc");
    for (int j = 0; j < i; j++)
      if (pages[i] == pages[j])
        panic("kalloc duplicate");
  }
  if (kalloc_free_pages() != expected - 3)
    panic("kalloc count");

  for (int i = 0; i < 3; i++)
    kfree(pages[i]);
  for (int i = 0; i < 3; i++) {
    reused[i] = kalloc();
    if (reused[i] != pages[2 - i])
      panic("kalloc reuse");
  }
  for (int i = 0; i < 3; i++)
    kfree(reused[i]);

  if (kalloc_free_pages() != expected)
    panic("kalloc restore");
}

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

  allocator_selftest();
}
