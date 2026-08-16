#include "mmix.h"
#include "cpu.h"
#include "early_print.h"

struct cpu cpus[NCPU];
uint64 mmix_trap_rk_shadow;

void
mmix_intr_mask_write(uint64 mask)
{
  if (mask == 0) {
    mmix_rk_write(0);
    mmix_trap_rk_shadow = 0;
  } else {
    mmix_trap_rk_shadow = mask;
    mmix_rk_write(mask);
  }
}

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
  mmix_intr_mask_write(mmix_rk_read() & ~MMIX_KERNEL_INTC_MASK);
}

void
intr_on(void)
{
  mmix_intr_mask_write(mmix_rk_read() | MMIX_KERNEL_INTC_MASK);
}

_Static_assert(NCPU == BOOT_CPU_COUNT,
               "the kernel must provide exactly one CPU structure");
