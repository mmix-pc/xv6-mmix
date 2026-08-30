#include "mmix.h"
#include "boot.h"
#include "cpu.h"
#include "defs.h"
#include "kcontext.h"
#include "memlayout.h"

static void cpu_secondary_idle(uint64) __attribute__((noreturn));

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

void
cpu_secondary_enter(void (*ready)(void))
{
  struct context bootstrap_context;
  struct cpu *c = mycpu();
  int id = cpuid();

  intr_off();
  if (ready == 0 || id == BOOT_CPU_ID || c->proc != 0 ||
      c->context.state != 0 || c->noff != 0 || c->intena != 0 || intr_get())
    panic("secondary context");
  kcontext_prepare_arg(&c->context, MMIX_CONTEXT_SCHEDULER_SLOT(id),
                       cpu_secondary_idle, (uint64)ready);
  swtch(&bootstrap_context, &c->context);
  panic("secondary context returned");
}

static void
cpu_secondary_idle(uint64 ready_address)
{
  void (*ready)(void) = (void (*)(void))ready_address;

  ready();
  if (boot_wait_for_scheduler_release() < 0)
    panic("scheduler release");
  scheduler();
}
