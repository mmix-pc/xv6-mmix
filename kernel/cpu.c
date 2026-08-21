#include "mmix.h"
#include "cpu.h"

uint64 mmix_trap_rk_shadow;

void
mmix_intr_mask_write(uint64 mask)
{
  uint64 active = mmix_rk_read();

  if ((active & MMIX_KERNEL_INTC_MASK) != 0 &&
      (mask & MMIX_KERNEL_INTC_MASK) == 0) {
    // Close external delivery before publishing a shadow without INTC.
    mmix_rk_write(mask);
    mmix_trap_rk_shadow = mask;
  } else {
    // Prepare trap entry before opening external delivery.
    mmix_trap_rk_shadow = mask;
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
  mmix_intr_mask_write(mmix_rk_read() & ~MMIX_KERNEL_INTC_MASK);
}

void
intr_on(void)
{
  mmix_intr_mask_write(mmix_rk_read() | MMIX_KERNEL_INTC_MASK);
}

void
cpu_idle(void)
{
  asm volatile("SWYM 0, 0, 0");
}
