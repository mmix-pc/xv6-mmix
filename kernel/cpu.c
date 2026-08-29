#include "mmix.h"
#include "cpu.h"

void
mmix_intr_mask_write(uint64 mask)
{
  struct cpu *c = mycpu();
  uint64 active = mmix_rk_read();

  if ((active & MMIX_KERNEL_INTERRUPT_MASK) != 0 &&
      (mask & MMIX_KERNEL_INTERRUPT_MASK) == 0) {
    // Close dynamic delivery before publishing a restrictive shadow.
    mmix_rk_write(mask);
    c->trap.rk_shadow = mask;
  } else {
    // Prepare this CPU's trap entry before opening dynamic delivery.
    c->trap.rk_shadow = mask;
    mmix_rk_write(mask);
  }
}

int
intr_get(void)
{
  return mmix_intr_get();
}

void
intr_off(void)
{
  mmix_intr_mask_write(mmix_rk_read() & ~MMIX_KERNEL_INTERRUPT_MASK);
}

void
intr_on(void)
{
  mmix_intr_mask_write(mmix_rk_read() | MMIX_KERNEL_INTERRUPT_MASK);
}

void
cpu_idle(void)
{
  asm volatile("SWYM 0, 0, 0");
}
