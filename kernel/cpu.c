#include "mmix.h"
#include "boot.h"
#include "cpu.h"
#include "defs.h"
#include "kcontext.h"
#include "memlayout.h"
#include "platform.h"
#include "physmem.h"
#include "kalloc.h"

static void cpu_secondary_idle(uint64) __attribute__((noreturn));
static void cpu_context_ready(uint64) __attribute__((noreturn));
static int initial_stack_detached[NCPU];
static uint64 boot_stack_departures;

uint64
cpu_boot_stack_departures(void)
{
  return __atomic_load_n(&boot_stack_departures, __ATOMIC_ACQUIRE);
}

static void
cpu_retire_boot_context(void)
{
  uint id = cpuid();

  if (!kcontext_current_valid(&mycpu()->context,
                              MMIX_CONTEXT_SCHEDULER_SLOT(id)))
    panic("CPU permanent stack");
  initial_stack_detached[id] = 1;
  if (cpu_reclaim_initial_stack(id) != PHYSMEM_OK)
    panic("CPU initial stack");
  // This separate acknowledgment proves the software stack was abandoned by
  // irreversible entry. Copied entry addresses do not keep its pages live.
  __atomic_fetch_or(&boot_stack_departures, 1ULL << id, __ATOMIC_RELEASE);
}

int
cpu_reclaim_initial_stack(uint32 id)
{
  struct cpu *c = mycpu();
  struct physmem_release released;
  int status;

  if (id != cpuid() || id >= platform_cpu_count())
    return PHYSMEM_BAD_ARGUMENT;
  if (__atomic_load_n(&mmix_startup.context_transfers[id],
                      __ATOMIC_ACQUIRE) != 0)
    return PHYSMEM_ALREADY_RELEASED;
  if (!initial_stack_detached[id] ||
      __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) !=
        MMIX_STARTUP_GLOBAL_READY ||
      (__atomic_load_n(&mmix_startup.online, __ATOMIC_ACQUIRE) &
       (1ULL << id)) == 0 ||
      mmix_rv_read() != MMIX_KERNEL_RV || intr_get() || c->noff != 0 ||
      c->proc != 0 || c->trap.active != 0 ||
      !kcontext_current_valid(&c->context, MMIX_CONTEXT_SCHEDULER_SLOT(id)))
    return PHYSMEM_NOT_READY;

  status = physmem_release_cpu_stack(id, &released);
  if (status != PHYSMEM_OK)
    return status;
  kalloc_publish_release(&released);
  // The new rO/rS and software stack were checked after the final UNSAVE.
  // Publish completion only after every newly eligible page is allocatable.
  __atomic_store_n(&mmix_startup.context_transfers[id], 1, __ATOMIC_RELEASE);
  return PHYSMEM_OK;
}

void
cpu_context_enter(void (*ready)(void))
{
  struct cpu *c = mycpu();

  intr_off();
  if (ready == 0 || c->context.state != 0 || c->proc != 0 || c->noff != 0)
    panic("CPU context");
  kcontext_prepare_arg(&c->context, MMIX_CONTEXT_SCHEDULER_SLOT(cpuid()),
                       cpu_context_ready, (uint64)ready);
  kcontext_enter(&c->context);
}

static void
cpu_context_ready(uint64 ready_address)
{
  void (*ready)(void) = (void (*)(void))ready_address;

  cpu_retire_boot_context();
  ready();
  panic("CPU context returned");
}

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
  struct cpu *c = mycpu();
  int id = cpuid();

  intr_off();
  if (ready == 0 || id == BOOT_CPU_ID || c->proc != 0 ||
      c->context.state != 0 || c->noff != 0 || c->intena != 0 || intr_get())
    panic("secondary context");
  kcontext_prepare_arg(&c->context, MMIX_CONTEXT_SCHEDULER_SLOT(id),
                       cpu_secondary_idle, (uint64)ready);
  kcontext_enter(&c->context);
}

static void
cpu_secondary_idle(uint64 ready_address)
{
  void (*ready)(void) = (void (*)(void))ready_address;

  cpu_retire_boot_context();
  ready();
  if (boot_wait_for_scheduler_release() < 0)
    panic("scheduler release");
  scheduler();
}
