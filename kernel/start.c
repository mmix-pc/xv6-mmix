#include "boot.h"
#include "cpu.h"
#include "mmix.h"
#include "defs.h"
#include "diagnostic.h"
#include "early_uart.h"
#include "kcontext.h"
#include "vm.h"

void main(void) __attribute__((noreturn));

struct mmix_boot_state mmix_boot;
struct mmix_boot_handoff mmix_boot_handoffs[MMIX_MAX_CPUS];
struct mmix_startup_control mmix_startup;

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
startup_publish_context_transfer(uint64 cpu_id)
{
  uint64 expected = 0;

  if (!__atomic_compare_exchange_n(&mmix_startup.context_transfers[cpu_id],
                                   &expected, 1, 0, __ATOMIC_RELEASE,
                                   __ATOMIC_RELAXED)) {
    startup_fail(MMIX_STARTUP_FAILURE_DUPLICATE_CONTEXT);
    return -1;
  }
  startup_set_stage(cpu_id, MMIX_CPU_STAGE_CONTEXT_READY);
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
startup_validate_handoffs(uint64 cpu_count, uint64 bootinfo_pa)
{
  for (uint64 cpu_id = 0; cpu_id < cpu_count; cpu_id++) {
    const struct mmix_boot_handoff *handoff = &mmix_boot_handoffs[cpu_id];
    uint64 stage = __atomic_load_n(&mmix_startup.cpu_stage[cpu_id],
                                   __ATOMIC_ACQUIRE);

    if (handoff->startup_cpu_id != cpu_id ||
        handoff->bootinfo_pa != bootinfo_pa ||
        handoff->software_stack_base != BOOT_STACK_BASE(cpu_id) ||
        handoff->software_stack_top != BOOT_STACK_TOP(cpu_id) ||
        handoff->register_stack_base != BOOT_REGISTER_STACK_BASE(cpu_id) ||
        handoff->register_stack_limit != BOOT_REGISTER_STACK_LIMIT(cpu_id) ||
        (stage != MMIX_CPU_STAGE_ARRIVED &&
         stage != MMIX_CPU_STAGE_WAIT_GLOBAL)) {
      startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
      return -1;
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

static void boot_secondary_context_ready(void);
static void secondary_wait_for_global(uint64 cpu_id, uint64 bootinfo_pa)
  __attribute__((noreturn));

static void
secondary_wait_for_global(uint64 cpu_id, uint64 bootinfo_pa)
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
          __atomic_load_n(&mmix_startup.ready_cookie, __ATOMIC_RELAXED) !=
            MMIX_STARTUP_READY_COOKIE ||
          mmix_boot.bootinfo_status != MMIX_BOOTINFO_OK ||
          cpu_id >= mmix_boot.info.cpu_count ||
          bootinfo_pa != mmix_boot.bootinfo_pa || kernel_pagetable == 0) {
        startup_fail(MMIX_STARTUP_FAILURE_PREMATURE_PUBLICATION);
        startup_terminal();
      }
      startup_set_stage(cpu_id, MMIX_CPU_STAGE_GLOBAL_ACQUIRED);
      kvminithart();
      trapinithart();
      if (cpuid() != (int)cpu_id || mycpu() != &cpus[cpu_id] ||
          mycpu()->proc != 0 || mycpu()->context.state != 0 ||
          mycpu()->noff != 0 || mycpu()->intena != 0 ||
          mmix_rv_read() != kernel_pagetable->rv || mmix_rk_read() != 0 ||
          intr_get()) {
        startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
        startup_terminal();
      }
      startup_set_stage(cpu_id, MMIX_CPU_STAGE_LOCAL_READY);
      cpu_secondary_enter(boot_secondary_context_ready);
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

static void
boot_secondary_context_ready(void)
{
  uint64 cpu_id = cpuid();
  struct cpu *c = mycpu();
  uint64 stage;

  if (cpu_id == BOOT_CPU_ID || cpu_id >= mmix_boot.info.cpu_count)
    goto fail;
  stage = __atomic_load_n(&mmix_startup.cpu_stage[cpu_id], __ATOMIC_ACQUIRE);
  if (stage != MMIX_CPU_STAGE_LOCAL_READY || c != &cpus[cpu_id] ||
      c->proc != 0 || c->noff != 0 || c->intena != 0 ||
      c->trap.active != 0 || c->trap.rk_shadow != 0 ||
      !kcontext_current_valid(&c->context,
                              MMIX_CONTEXT_SCHEDULER_SLOT(cpu_id)) ||
      mmix_rv_read() != kernel_pagetable->rv || mmix_rk_read() != 0 ||
      intr_get() || mmix_rt_read() == 0 ||
      mmix_rt_read() != mmix_rtt_read())
    goto fail;
  if (startup_publish_context_transfer(cpu_id) < 0 ||
      startup_publish_online(cpu_id) < 0)
    startup_terminal();
  startup_set_stage(cpu_id, MMIX_CPU_STAGE_SECONDARY_IDLE);
  return;

fail:
  startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
  startup_terminal();
}

int
boot_wait_for_online(void)
{
  uint64 cpu_count = mmix_boot.info.cpu_count;
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
      __atomic_load_n(&mmix_startup.failure, __ATOMIC_RELAXED) !=
        MMIX_STARTUP_FAILURE_NONE ||
      __atomic_load_n(&mmix_startup.ready_cookie, __ATOMIC_RELAXED) !=
        MMIX_STARTUP_READY_COOKIE ||
      __atomic_load_n(&mmix_startup.context_transfers[BOOT_CPU_ID],
                      __ATOMIC_RELAXED) != 0 ||
      cpuid() != BOOT_CPU_ID || mycpu() != &cpus[BOOT_CPU_ID] ||
      mycpu()->proc != 0 || mycpu()->context.state != 0 ||
      mycpu()->noff != 0 || mycpu()->intena != 0 ||
      mmix_rv_read() != kernel_pagetable->rv ||
      mmix_rk_read() != MMIX_KERNEL_PROGRAM_MASK || intr_get()) {
    startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
    return -1;
  }

  startup_set_stage(BOOT_CPU_ID, MMIX_CPU_STAGE_LOCAL_READY);
  if (startup_publish_online(BOOT_CPU_ID) < 0)
    return -1;

  for (;;) {
    uint64 online = __atomic_load_n(&mmix_startup.online, __ATOMIC_ACQUIRE);
    int idle = 1;

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
      uint64 transfers =
        __atomic_load_n(&mmix_startup.context_transfers[cpu_id],
                        __ATOMIC_ACQUIRE);

      if (transfers != 1) {
        startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
        return -1;
      }
      if (stage == MMIX_CPU_STAGE_CONTEXT_READY ||
          stage == MMIX_CPU_STAGE_ONLINE) {
        idle = 0;
        continue;
      }
      if (stage != MMIX_CPU_STAGE_SECONDARY_IDLE) {
        startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
        return -1;
      }
    }
    for (uint64 cpu_id = cpu_count; cpu_id < MMIX_MAX_CPUS; cpu_id++) {
      if (__atomic_load_n(&mmix_startup.context_transfers[cpu_id],
                          __ATOMIC_RELAXED) != 0) {
        startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
        return -1;
      }
    }
    if (!idle) {
      asm volatile("SWYM 0, 0, 0" ::: "memory");
      continue;
    }
    diagnostic_startup(cpu_count, online);
    startup_set_stage(BOOT_CPU_ID, MMIX_CPU_STAGE_SERVICE);
    return 0;
  }
}

int
boot_publish_global_ready(void)
{
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
      kernel_pagetable == 0) {
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
start(uint64 startup_cpu_id, uint64 bootinfo_pa)
{
  struct mmix_boot_state decoded_boot;
  struct mmix_boot_handoff *handoff;

  // _entry performs this check before selecting a stack. Retain a C-side
  // boundary check so future callers cannot index the handoff array unsafely.
  if (startup_cpu_id >= MMIX_MAX_CPUS)
    startup_terminal();

  startup_set_stage(startup_cpu_id, MMIX_CPU_STAGE_ENTRY);
  handoff = &mmix_boot_handoffs[startup_cpu_id];
  handoff->startup_cpu_id = startup_cpu_id;
  handoff->bootinfo_pa = bootinfo_pa;
  handoff->software_stack_base = BOOT_STACK_BASE(startup_cpu_id);
  handoff->software_stack_top = BOOT_STACK_TOP(startup_cpu_id);
  handoff->register_stack_base = BOOT_REGISTER_STACK_BASE(startup_cpu_id);
  handoff->register_stack_limit = BOOT_REGISTER_STACK_LIMIT(startup_cpu_id);

  if ((uint64)cpuid() != startup_cpu_id ||
      mycpu() != &cpus[startup_cpu_id]) {
    startup_fail(MMIX_STARTUP_FAILURE_TOPOLOGY);
    startup_terminal();
  }
  if (startup_publish_arrival(startup_cpu_id) < 0)
    startup_terminal();

  if (startup_cpu_id != BOOT_CPU_ID)
    secondary_wait_for_global(startup_cpu_id, bootinfo_pa);

  decoded_boot.startup_cpu_id = startup_cpu_id;
  decoded_boot.bootinfo_pa = bootinfo_pa;
  decoded_boot.bootinfo_status =
    bootinfo_decode(startup_cpu_id, bootinfo_pa, &decoded_boot.info);

  // CPU 0 uses the fixed early UART to diagnose failures before publication.
  early_uart_init();
  diagnostic_boot(&decoded_boot);
  if (decoded_boot.bootinfo_status != MMIX_BOOTINFO_OK) {
    startup_fail(MMIX_STARTUP_FAILURE_BOOTINFO);
    panic("bootinfo");
  }
  if (startup_begin_collection() < 0 ||
      startup_wait_for_arrivals(
        startup_expected_mask(decoded_boot.info.cpu_count)) < 0 ||
      startup_validate_handoffs(decoded_boot.info.cpu_count, bootinfo_pa) < 0 ||
      startup_claim_global_initialization() < 0)
    panic("SMP startup");

  mmix_boot = decoded_boot;
  main();
}
