#include "boot.h"
#include "cpu.h"
#include "fdt.h"
#include "mmix.h"
#include "defs.h"
#include "diagnostic.h"
#include "intc.h"
#include "ipi.h"
#include "kcontext.h"
#include "platform.h"
#include "physmem.h"
#include "kalloc.h"
#include "timer.h"
#include "vm.h"

void main(void) __attribute__((noreturn));

struct mmix_boot_handoff mmix_boot_handoffs[MMIX_MAX_CPUS];
struct mmix_startup_control mmix_startup;
static int fdt_readers_done;

static void
startup_set_stage(uint64 cpu_id, enum mmix_cpu_startup_stage stage)
{
  __atomic_store_n(&mmix_startup.cpu_stage[cpu_id], (uint64)stage,
                   __ATOMIC_RELEASE);
}

static void
startup_fail(enum mmix_startup_failure failure)
{
  uint64 expected = MMIX_STARTUP_FAILURE_NONE;

  __atomic_compare_exchange_n(&mmix_startup.failure, &expected,
                              (uint64)failure, 0, __ATOMIC_RELAXED,
                              __ATOMIC_RELAXED);
  __atomic_store_n(&mmix_startup.state, MMIX_STARTUP_FAILED,
                   __ATOMIC_RELEASE);
}

static void startup_terminal(void) __attribute__((noreturn));

static void
startup_terminal(void)
{
  for (;;)
    asm volatile("SWYM 0, 0, 0" ::: "memory");
}

static uint64
startup_expected_mask(uint64 cpu_count)
{
  return (1ULL << cpu_count) - 1;
}

static int
startup_publish_arrival(uint64 cpu_id)
{
  uint64 old;

  startup_set_stage(cpu_id, MMIX_CPU_STAGE_ARRIVED);
  old = __atomic_fetch_or(&mmix_startup.arrived, 1ULL << cpu_id,
                          __ATOMIC_RELEASE);
  if (old & (1ULL << cpu_id)) {
    startup_fail(MMIX_STARTUP_FAILURE_DUPLICATE_ARRIVAL);
    return -1;
  }
  return 0;
}

static int
startup_publish_online(uint64 cpu_id)
{
  uint64 old;

  startup_set_stage(cpu_id, MMIX_CPU_STAGE_ONLINE);
  old = __atomic_fetch_or(&mmix_startup.online, 1ULL << cpu_id,
                          __ATOMIC_RELEASE);
  if (old & (1ULL << cpu_id)) {
    startup_fail(MMIX_STARTUP_FAILURE_DUPLICATE_ONLINE);
    return -1;
  }
  return 0;
}

static int
startup_publish_interrupt_ready(uint64 cpu_id)
{
  uint64 old;

  startup_set_stage(cpu_id, MMIX_CPU_STAGE_INTERRUPT_READY);
  old = __atomic_fetch_or(&mmix_startup.interrupt_ready, 1ULL << cpu_id,
                          __ATOMIC_RELEASE);
  if (old & (1ULL << cpu_id)) {
    startup_fail(MMIX_STARTUP_FAILURE_DUPLICATE_INTERRUPT_READY);
    return -1;
  }
  return 0;
}

static int
startup_begin_collection(void)
{
  uint64 expected = MMIX_STARTUP_RESET;
  uint64 generation = 0;

  if (!__atomic_compare_exchange_n(&mmix_startup.generation, &generation,
                                   MMIX_STARTUP_GENERATION, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    startup_fail(MMIX_STARTUP_FAILURE_GENERATION);
    return -1;
  }
  if (!__atomic_compare_exchange_n(&mmix_startup.state, &expected,
                                   MMIX_STARTUP_COLLECTING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    startup_fail(MMIX_STARTUP_FAILURE_PREMATURE_PUBLICATION);
    return -1;
  }
  return 0;
}

static int
startup_wait_for_arrivals(uint64 expected_mask)
{
  for (;;) {
    uint64 state = __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE);
    uint64 arrived = __atomic_load_n(&mmix_startup.arrived, __ATOMIC_ACQUIRE);

    if (state == MMIX_STARTUP_FAILED)
      return -1;
    if (state != MMIX_STARTUP_COLLECTING || (arrived & ~expected_mask) != 0) {
      startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
      return -1;
    }
    if (arrived == expected_mask)
      return 0;
    asm volatile("SWYM 0, 0, 0" ::: "memory");
  }
}

static int
startup_validate_handoffs(uint64 fdt_address)
{
  uint32 cpu_count = platform_cpu_count();

  for (uint64 cpu_id = 0; cpu_id < cpu_count; cpu_id++) {
    const struct mmix_boot_handoff *handoff = &mmix_boot_handoffs[cpu_id];
    struct platform_physical_range stack;
    uint64 stage = __atomic_load_n(&mmix_startup.cpu_stage[cpu_id],
                                   __ATOMIC_ACQUIRE);

    if (platform_cpu_initial_stack(cpu_id, &stack) != PLATFORM_OK) {
      startup_fail(MMIX_STARTUP_FAILURE_REGISTER_STACK);
      return -1;
    }
    if (handoff->startup_cpu_id != cpu_id ||
        handoff->fdt_address != fdt_address) {
      startup_fail(MMIX_STARTUP_FAILURE_FDT);
      return -1;
    }
    if (handoff->entry_rl != 2) {
      startup_fail(MMIX_STARTUP_FAILURE_ENTRY_RL);
      return -1;
    }
    if (handoff->entry_ro != stack.physical_base ||
        handoff->entry_rs != stack.physical_base ||
        stack.size != INITIAL_REGISTER_STACK_SIZE) {
      startup_fail(MMIX_STARTUP_FAILURE_REGISTER_STACK);
      return -1;
    }
    if (handoff->software_stack_base != boot_stack_base(cpu_id) ||
        handoff->software_stack_top != boot_stack_top(cpu_id) ||
        (stage != MMIX_CPU_STAGE_ARRIVED &&
         stage != MMIX_CPU_STAGE_WAIT_GLOBAL)) {
      startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
      return -1;
    }
  }
  for (uint64 cpu_id = 0; cpu_id < cpu_count; cpu_id++) {
    uint64 left = mmix_boot_handoffs[cpu_id].entry_ro;

    for (uint64 other = 0; other < cpu_id; other++) {
      uint64 right = mmix_boot_handoffs[other].entry_ro;

      if ((left < right && INITIAL_REGISTER_STACK_SIZE > right - left) ||
          (left >= right && INITIAL_REGISTER_STACK_SIZE > left - right)) {
        startup_fail(MMIX_STARTUP_FAILURE_REGISTER_STACK);
        return -1;
      }
    }
  }
  return 0;
}

static int
startup_claim_global_initialization(void)
{
  uint64 expected = 0;

  if (!__atomic_compare_exchange_n(&mmix_startup.global_owner, &expected,
                                   BOOT_CPU_ID + 1, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    startup_fail(MMIX_STARTUP_FAILURE_DUPLICATE_INITIALIZER);
    return -1;
  }
  expected = 0;
  if (!__atomic_compare_exchange_n(&mmix_startup.global_initializer_count,
                                   &expected, 1, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    startup_fail(MMIX_STARTUP_FAILURE_DUPLICATE_INITIALIZER);
    return -1;
  }
  expected = MMIX_STARTUP_COLLECTING;
  if (!__atomic_compare_exchange_n(&mmix_startup.state, &expected,
                                   MMIX_STARTUP_INITIALIZING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    startup_fail(MMIX_STARTUP_FAILURE_PREMATURE_PUBLICATION);
    return -1;
  }
  startup_set_stage(BOOT_CPU_ID, MMIX_CPU_STAGE_GLOBAL_INIT);
  return 0;
}

static void secondary_wait_for_global(uint64 cpu_id, uint64 fdt_address)
  __attribute__((noreturn));

static void
secondary_wait_for_global(uint64 cpu_id, uint64 fdt_address)
{
  startup_set_stage(cpu_id, MMIX_CPU_STAGE_WAIT_GLOBAL);
  for (;;) {
    uint64 state = __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE);

    if (state == MMIX_STARTUP_FAILED)
      startup_terminal();
    if (state == MMIX_STARTUP_GLOBAL_READY) {
      if (__atomic_load_n(&mmix_startup.generation, __ATOMIC_RELAXED) !=
            MMIX_STARTUP_GENERATION ||
          __atomic_load_n(&mmix_startup.failure, __ATOMIC_RELAXED) !=
            MMIX_STARTUP_FAILURE_NONE ||
          __atomic_load_n(&mmix_startup.global_owner, __ATOMIC_RELAXED) !=
            BOOT_CPU_ID + 1 ||
          __atomic_load_n(&mmix_startup.global_initializer_count,
                          __ATOMIC_RELAXED) != 1 ||
          __atomic_load_n(&mmix_startup.platform_publications,
                          __ATOMIC_RELAXED) != 1 ||
          __atomic_load_n(&mmix_startup.ready_cookie, __ATOMIC_RELAXED) !=
            MMIX_STARTUP_READY_COOKIE ||
          cpu_id >= platform_cpu_count() ||
          fdt_address != platform_fdt_physical_address()) {
        startup_fail(MMIX_STARTUP_FAILURE_PREMATURE_PUBLICATION);
        startup_terminal();
      }
      startup_set_stage(cpu_id, MMIX_CPU_STAGE_GLOBAL_ACQUIRED);
      if (startup_publish_online(cpu_id) < 0)
        startup_terminal();
      startup_terminal();
    }
    if (state != MMIX_STARTUP_RESET &&
        state != MMIX_STARTUP_COLLECTING &&
        state != MMIX_STARTUP_INITIALIZING) {
      startup_fail(MMIX_STARTUP_FAILURE_PREMATURE_PUBLICATION);
      startup_terminal();
    }
    asm volatile("SWYM 0, 0, 0" ::: "memory");
  }
}

int
boot_wait_for_online(void)
{
  uint64 cpu_count = platform_cpu_count();
  uint64 expected_mask = startup_expected_mask(cpu_count);

  if (__atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) !=
        MMIX_STARTUP_GLOBAL_READY ||
      __atomic_load_n(&mmix_startup.generation, __ATOMIC_RELAXED) !=
        MMIX_STARTUP_GENERATION ||
      __atomic_load_n(&mmix_startup.arrived, __ATOMIC_RELAXED) !=
        expected_mask ||
      __atomic_load_n(&mmix_startup.global_owner, __ATOMIC_RELAXED) !=
        BOOT_CPU_ID + 1 ||
      __atomic_load_n(&mmix_startup.global_initializer_count,
                      __ATOMIC_RELAXED) != 1 ||
      __atomic_load_n(&mmix_startup.platform_publications,
                      __ATOMIC_RELAXED) != 1 ||
      __atomic_load_n(&mmix_startup.failure, __ATOMIC_RELAXED) !=
        MMIX_STARTUP_FAILURE_NONE ||
      __atomic_load_n(&mmix_startup.ready_cookie, __ATOMIC_RELAXED) !=
        MMIX_STARTUP_READY_COOKIE ||
      cpuid() != BOOT_CPU_ID) {
    startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
    return -1;
  }

  startup_set_stage(BOOT_CPU_ID, MMIX_CPU_STAGE_GLOBAL_ACQUIRED);
  if (startup_publish_online(BOOT_CPU_ID) < 0)
    return -1;

  for (;;) {
    uint64 online = __atomic_load_n(&mmix_startup.online, __ATOMIC_ACQUIRE);

    if (__atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) ==
        MMIX_STARTUP_FAILED)
      return -1;
    if ((online & ~expected_mask) != 0) {
      startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
      return -1;
    }
    if (online != expected_mask) {
      asm volatile("SWYM 0, 0, 0" ::: "memory");
      continue;
    }
    for (uint64 cpu_id = 1; cpu_id < cpu_count; cpu_id++) {
      uint64 stage = __atomic_load_n(&mmix_startup.cpu_stage[cpu_id],
                                     __ATOMIC_ACQUIRE);
      if (stage == MMIX_CPU_STAGE_ONLINE)
        continue;
      startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
      return -1;
    }
    for (uint64 cpu_id = cpu_count; cpu_id < MMIX_MAX_CPUS; cpu_id++) {
      if (__atomic_load_n(&mmix_startup.context_transfers[cpu_id],
                          __ATOMIC_RELAXED) != 0) {
        startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
        return -1;
      }
    }
    // Discovery retains only copied values. Every entry-address check has
    // completed; retained FDT addresses are provenance, never blob pointers.
    fdt_readers_done = 1;
    return 0;
  }
}

int
boot_reclaim_fdt(void)
{
  struct kalloc_stats stats;
  struct physmem_release released;
  int status;

  // The boot CPU owns this transition before any page table is constructed.
  // Reclaim first so no stale read-only FDT translation can survive reuse.
  if (cpuid() != BOOT_CPU_ID || !fdt_readers_done ||
      kernel_pagetable->rv != 0 ||
      __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) !=
        MMIX_STARTUP_GLOBAL_READY)
    return PHYSMEM_NOT_READY;
  kalloc_get_stats(&stats);
  if (stats.managed_pages == 0)
    return PHYSMEM_NOT_READY;
  status = physmem_release_fdt(&released);
  if (status == PHYSMEM_OK)
    kalloc_publish_release(&released);
  return status;
}

int
boot_reclaim_stacks(void)
{
  struct physmem_release released;
  struct cpu *c = mycpu();
  uint64 mask = platform_cpu_mask();
  int status;

  if (cpuid() != BOOT_CPU_ID || mask == 0 || intr_get() ||
      c->noff != 0 || c->trap.active != 0 || c->proc != 0 ||
      __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) !=
        MMIX_STARTUP_GLOBAL_READY ||
      __atomic_load_n(&mmix_startup.online, __ATOMIC_ACQUIRE) != mask ||
      cpu_boot_stack_departures() != mask ||
      !kcontext_current_valid(&c->context,
                              MMIX_CONTEXT_SCHEDULER_SLOT(BOOT_CPU_ID)))
    return PHYSMEM_NOT_READY;

  // No CPU can return to an entry frame after the complete departure mask.
  // Keep the whole range reserved until then, including unused CPU slots.
  status = physmem_release_boot_stacks(&released);
  if (status == PHYSMEM_OK)
    kalloc_publish_release(&released);
  return status;
}

int
boot_publish_interrupt_ready(void)
{
  uint64 cpu_id = cpuid();
  uint64 stage;
  uint64 entries;
  uint64 returns;
  uint64 enabled;
  uint64 expected_enabled;
  uint32 timer_irq_number;
  struct cpu *c = mycpu();

  if (cpu_id >= platform_cpu_count() || c != &cpus[cpu_id] ||
      timer_irq(&timer_irq_number) != MMIX_TIMER_OK ||
      !intr_get())
    goto fail;
  push_off();
  stage = __atomic_load_n(&mmix_startup.cpu_stage[cpu_id], __ATOMIC_ACQUIRE);
  entries = __atomic_load_n(&c->trap.interrupt_entries, __ATOMIC_ACQUIRE);
  returns = __atomic_load_n(&c->trap.interrupt_returns, __ATOMIC_ACQUIRE);
  if (stage != MMIX_CPU_STAGE_ONLINE ||
      (__atomic_load_n(&mmix_startup.online, __ATOMIC_ACQUIRE) &
       (1ULL << cpu_id)) == 0 ||
      (cpu_id != BOOT_CPU_ID &&
       __atomic_load_n(&mmix_startup.context_transfers[cpu_id],
                       __ATOMIC_ACQUIRE) != 1) ||
      intr_get() || (mmix_rk_read() & MMIX_KERNEL_INTERRUPT_MASK) != 0 ||
      c->trap.rk_shadow != mmix_rk_read() || c->trap.active != 0 ||
      c->proc != 0 || c->noff != 1 || c->intena != 1 ||
      timer_ticks() == 0 || entries == 0 || entries != returns)
    goto fail;

  for (uint32 word = 0; word < INTC_WORD_COUNT; word++)
    if (intc_enabled(word, &enabled) != MMIX_INTC_OK ||
        intc_runtime_mask(timer_irq_number, word, &expected_enabled) !=
          MMIX_INTC_OK || enabled != expected_enabled)
      goto fail;

  printk("interrupt-ready: cpu=%d timer=%llu noff=%d handlers=%llu/%llu\n",
         (int)cpu_id, (unsigned long long)timer_ticks(), c->noff - 1,
         (unsigned long long)entries, (unsigned long long)returns);
  if (startup_publish_interrupt_ready(cpu_id) < 0)
    return -1;
  pop_off();
  return 0;

fail:
  startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
  return -1;
}

int
boot_wait_for_interrupt_ready(void)
{
  uint64 cpu_count = platform_cpu_count();
  uint64 expected_mask = startup_expected_mask(cpu_count);

  if (cpuid() != BOOT_CPU_ID ||
      __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) !=
        MMIX_STARTUP_GLOBAL_READY)
    goto fail;

  for (;;) {
    uint64 ready =
      __atomic_load_n(&mmix_startup.interrupt_ready, __ATOMIC_ACQUIRE);
    int all_idle = 1;

    if (__atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) ==
        MMIX_STARTUP_FAILED)
      return -1;
    if ((ready & ~expected_mask) != 0)
      goto fail;
    if (ready != expected_mask) {
      cpu_idle();
      continue;
    }
    for (uint64 cpu_id = 1; cpu_id < cpu_count; cpu_id++) {
      uint64 stage = __atomic_load_n(&mmix_startup.cpu_stage[cpu_id],
                                     __ATOMIC_ACQUIRE);

      if (stage == MMIX_CPU_STAGE_INTERRUPT_READY) {
        all_idle = 0;
        continue;
      }
      if (stage != MMIX_CPU_STAGE_SECONDARY_IDLE ||
          cpus[cpu_id].proc != 0)
        goto fail;
    }
    if (!all_idle) {
      cpu_idle();
      continue;
    }
    startup_set_stage(BOOT_CPU_ID, MMIX_CPU_STAGE_SERVICE);
    return 0;
  }

fail:
  startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
  return -1;
}

int
boot_release_schedulers(void)
{
  uint64 cpu_count = platform_cpu_count();
  uint64 expected_mask = startup_expected_mask(cpu_count);

  if (cpuid() != BOOT_CPU_ID || intr_get() || mycpu()->proc != 0 ||
      __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) !=
        MMIX_STARTUP_GLOBAL_READY ||
      __atomic_load_n(&mmix_startup.interrupt_ready, __ATOMIC_ACQUIRE) !=
        expected_mask ||
      __atomic_load_n(&mmix_startup.cpu_stage[BOOT_CPU_ID],
                      __ATOMIC_ACQUIRE) != MMIX_CPU_STAGE_SERVICE)
    goto fail;
  for (uint64 cpu_id = 0; cpu_id < cpu_count; cpu_id++) {
    uint64 expected_stage = cpu_id == BOOT_CPU_ID ?
      MMIX_CPU_STAGE_SERVICE : MMIX_CPU_STAGE_SECONDARY_IDLE;

    if (__atomic_load_n(&mmix_startup.cpu_stage[cpu_id],
                        __ATOMIC_ACQUIRE) != expected_stage ||
        cpus[cpu_id].proc != 0 || cpus[cpu_id].scheduler_entries != 0 ||
        cpus[cpu_id].scheduler_dispatches != 0)
      goto fail;
  }
  __atomic_store_n(&mmix_startup.state, MMIX_STARTUP_SCHEDULER_RELEASED,
                   __ATOMIC_RELEASE);
  return 0;

fail:
  startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
  return -1;
}

int
boot_wait_for_scheduler_release(void)
{
  uint64 cpu_id = cpuid();

  if (cpu_id == BOOT_CPU_ID || cpu_id >= platform_cpu_count() ||
      !intr_get() ||
      __atomic_load_n(&mmix_startup.cpu_stage[cpu_id],
                      __ATOMIC_ACQUIRE) != MMIX_CPU_STAGE_SECONDARY_IDLE)
    goto fail;
  for (;;) {
    uint64 state = __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE);

    if (state == MMIX_STARTUP_SCHEDULER_RELEASED)
      return 0;
    if (state == MMIX_STARTUP_FAILED)
      return -1;
    if (state != MMIX_STARTUP_GLOBAL_READY)
      goto fail;
    cpu_idle();
  }

fail:
  startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
  return -1;
}

int
boot_publish_scheduler_ready(void)
{
  struct cpu *c = mycpu();
  uint64 cpu_id = cpuid();
  uint64 expected_stage = cpu_id == BOOT_CPU_ID ?
    MMIX_CPU_STAGE_SERVICE : MMIX_CPU_STAGE_SECONDARY_IDLE;
  int memory_only = cpu_id == BOOT_CPU_ID && platform_cpu_count() == 1 &&
    __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) ==
      MMIX_STARTUP_GLOBAL_READY && cpu_boot_stack_departures() == 1 &&
    __atomic_load_n(&mmix_startup.interrupt_ready, __ATOMIC_ACQUIRE) == 0;

  if (memory_only)
    expected_stage = MMIX_CPU_STAGE_ONLINE;

  if (cpu_id >= platform_cpu_count() || c != &cpus[cpu_id] ||
      (!memory_only &&
       __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) !=
         MMIX_STARTUP_SCHEDULER_RELEASED) ||
      c->proc != 0 || c->scheduler_entries != 1 ||
      c->scheduler_dispatches != 0 || c->noff != 0 || c->trap.active != 0 ||
      c->trap.user_trapframe != 0 || intr_get() ||
      !kcontext_current_valid(&c->context,
                              MMIX_CONTEXT_SCHEDULER_SLOT(cpu_id)))
    goto fail;
  if (!__atomic_compare_exchange_n(&mmix_startup.cpu_stage[cpu_id],
                                   &expected_stage,
                                   MMIX_CPU_STAGE_SCHEDULER, 0,
                                   __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
    goto fail;
  if (memory_only)
    __atomic_store_n(&mmix_startup.state, MMIX_STARTUP_SCHEDULER_RELEASED,
                     __ATOMIC_RELEASE);
  printk("scheduler-ready: cpu=%d context=%p\n", (int)cpu_id,
         (void *)c->context.state);
  return 0;

fail:
  startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
  return -1;
}

static int
startup_publish_platform(void)
{
  uint64 publications = 0;

  if (cpuid() != BOOT_CPU_ID ||
      __atomic_load_n(&mmix_startup.state, __ATOMIC_ACQUIRE) !=
        MMIX_STARTUP_INITIALIZING ||
      __atomic_load_n(&mmix_startup.generation, __ATOMIC_RELAXED) !=
        MMIX_STARTUP_GENERATION ||
      __atomic_load_n(&mmix_startup.global_owner, __ATOMIC_RELAXED) !=
        BOOT_CPU_ID + 1 ||
      __atomic_load_n(&mmix_startup.global_initializer_count,
                      __ATOMIC_RELAXED) != 1 ||
      __atomic_load_n(&mmix_startup.failure, __ATOMIC_RELAXED) !=
        MMIX_STARTUP_FAILURE_NONE ||
      __atomic_load_n(&mmix_startup.ready_cookie, __ATOMIC_RELAXED) != 0 ||
      !__atomic_compare_exchange_n(&mmix_startup.platform_publications,
                                   &publications, 1, 0, __ATOMIC_RELAXED,
                                   __ATOMIC_RELAXED)) {
    startup_fail(MMIX_STARTUP_FAILURE_PREMATURE_PUBLICATION);
    return -1;
  }

  __atomic_store_n(&mmix_startup.ready_cookie, MMIX_STARTUP_READY_COOKIE,
                   __ATOMIC_RELAXED);
  startup_set_stage(BOOT_CPU_ID, MMIX_CPU_STAGE_GLOBAL_ACQUIRED);
  __atomic_store_n(&mmix_startup.state, MMIX_STARTUP_GLOBAL_READY,
                   __ATOMIC_RELEASE);
  return 0;
}

void
start(uint64 startup_cpu_id, uint64 fdt_address, uint64 entry_rl,
      uint64 entry_ro, uint64 entry_rs)
{
  struct fdt fdt;
  struct mmix_boot_handoff *handoff;
  int decode_status = PLATFORM_OK;

  // _entry performs this check before selecting a stack. Retain a C-side
  // boundary check so future callers cannot index the handoff array unsafely.
  if (startup_cpu_id >= MMIX_MAX_CPUS)
    startup_terminal();

  startup_set_stage(startup_cpu_id, MMIX_CPU_STAGE_ENTRY);
  handoff = &mmix_boot_handoffs[startup_cpu_id];
  handoff->startup_cpu_id = startup_cpu_id;
  handoff->fdt_address = fdt_address;
  handoff->entry_rl = entry_rl;
  handoff->entry_ro = entry_ro;
  handoff->entry_rs = entry_rs;
  handoff->software_stack_base = boot_stack_base(startup_cpu_id);
  handoff->software_stack_top = boot_stack_top(startup_cpu_id);

  if (startup_cpu_id == BOOT_CPU_ID)
    early_uart_init();

  if ((uint64)cpuid() != startup_cpu_id ||
      mycpu() != &cpus[startup_cpu_id]) {
    startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
    goto failed;
  }
  if (entry_rl != 2) {
    startup_fail(MMIX_STARTUP_FAILURE_ENTRY_RL);
    goto failed;
  }
  if (entry_ro == 0 || entry_ro != entry_rs ||
      (entry_ro & (MMIX_PAGE_SIZE - 1)) != 0) {
    startup_fail(MMIX_STARTUP_FAILURE_REGISTER_STACK);
    goto failed;
  }
  if (startup_publish_arrival(startup_cpu_id) < 0)
    goto failed;

  if (startup_cpu_id != BOOT_CPU_ID)
    secondary_wait_for_global(startup_cpu_id, fdt_address);

  if (startup_begin_collection() < 0)
    goto failed;
  decode_status = fdt_open(&fdt, (const void *)fdt_address, FDT_MAX_SIZE);
  if (decode_status != FDT_OK ||
      (decode_status = platform_discover(&fdt, fdt_address)) != PLATFORM_OK) {
    startup_fail(MMIX_STARTUP_FAILURE_PLATFORM);
    goto failed;
  }
  if (startup_wait_for_arrivals(
        startup_expected_mask(platform_cpu_count())) < 0 ||
      startup_validate_handoffs(fdt_address) < 0 ||
      startup_claim_global_initialization() < 0)
    goto failed;

  if (startup_publish_platform() < 0)
    goto failed;
  main();

failed:
  if (startup_cpu_id == BOOT_CPU_ID) {
    diagnostic_startup_failure(
      __atomic_load_n(&mmix_startup.failure, __ATOMIC_RELAXED),
      decode_status);
    panic("SMP startup");
  }
  startup_terminal();
}
