#include "mmix.h"
#include "cpu.h"
#include "early_print.h"

struct cpu cpus[NCPU];

// The current kernel boots exactly one CPU and assigns it ID 0.
int
cpuid(void)
{
  return BOOT_CPU_ID;
}

// Callers keep dynamic interrupts masked while using CPU-local state.
struct cpu *
mycpu(void)
{
  return &cpus[BOOT_CPU_ID];
}

int
intr_get(void)
{
  return mmix_intr_get();
}

void
intr_off(void)
{
  mmix_intr_off();
}

// Dynamic traps do not have a handler yet, so enabling them is unsafe.
void
intr_on(void)
{
  panic("intr_on");
}

_Static_assert(NCPU == BOOT_CPU_COUNT,
               "the kernel must provide exactly one CPU structure");
