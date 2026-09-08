#ifndef XV6_MMIX_BOOT_H
#define XV6_MMIX_BOOT_H

#include "types.h"
#include "memlayout.h"

// Immutable entry data retained separately before the SMP startup barrier can
// publish the canonical boot description.
struct mmix_boot_handoff {
  uint64 startup_cpu_id;
  uint64 fdt_address;
  uint64 entry_rl;
  uint64 entry_ro;
  uint64 entry_rs;
  uint64 software_stack_base;
  uint64 software_stack_top;
};

enum mmix_startup_state {
  MMIX_STARTUP_RESET = 0,
  MMIX_STARTUP_COLLECTING = 1,
  MMIX_STARTUP_INITIALIZING = 2,
  MMIX_STARTUP_GLOBAL_READY = 3,
  MMIX_STARTUP_SCHEDULER_RELEASED = 4,
  MMIX_STARTUP_FAILED = 255,
};

enum mmix_cpu_startup_stage {
  MMIX_CPU_STAGE_RESET = 0,
  MMIX_CPU_STAGE_ENTRY = 1,
  MMIX_CPU_STAGE_ARRIVED = 2,
  MMIX_CPU_STAGE_WAIT_GLOBAL = 3,
  MMIX_CPU_STAGE_GLOBAL_INIT = 4,
  MMIX_CPU_STAGE_GLOBAL_ACQUIRED = 5,
  MMIX_CPU_STAGE_LOCAL_READY = 6,
  MMIX_CPU_STAGE_CONTEXT_READY = 7,
  MMIX_CPU_STAGE_ONLINE = 8,
  MMIX_CPU_STAGE_INTERRUPT_READY = 9,
  MMIX_CPU_STAGE_SERVICE = 10,
  MMIX_CPU_STAGE_SECONDARY_IDLE = 11,
  MMIX_CPU_STAGE_SCHEDULER = 12,
};

enum mmix_startup_failure {
  MMIX_STARTUP_FAILURE_NONE = 0,
  MMIX_STARTUP_FAILURE_DUPLICATE_ARRIVAL = 2,
  MMIX_STARTUP_FAILURE_TOPOLOGY = 3,
  MMIX_STARTUP_FAILURE_GENERATION = 4,
  MMIX_STARTUP_FAILURE_DUPLICATE_INITIALIZER = 5,
  MMIX_STARTUP_FAILURE_PREMATURE_PUBLICATION = 6,
  MMIX_STARTUP_FAILURE_DUPLICATE_ONLINE = 7,
  MMIX_STARTUP_FAILURE_DUPLICATE_CONTEXT = 8,
  MMIX_STARTUP_FAILURE_DUPLICATE_INTERRUPT_READY = 9,
  MMIX_STARTUP_FAILURE_ENTRY_RL = 10,
  MMIX_STARTUP_FAILURE_FDT = 11,
  MMIX_STARTUP_FAILURE_REGISTER_STACK = 12,
  MMIX_STARTUP_FAILURE_PLATFORM = 13,
};

#define MMIX_STARTUP_GENERATION 1
#define MMIX_STARTUP_READY_COOKIE 0x4d4d495852454144ULL

// Guest-owned synchronization state retained for the lifetime of the kernel.
struct mmix_startup_control {
  uint64 generation;
  uint64 state;
  uint64 arrived;
  uint64 online;
  // Zero means unclaimed; a claimed owner is encoded as CPU ID plus one.
  uint64 global_owner;
  uint64 global_initializer_count;
  uint64 failure;
  uint64 ready_cookie;
  uint64 platform_publications;
  uint64 cpu_stage[MMIX_MAX_CPUS];
  uint64 context_transfers[MMIX_MAX_CPUS];
  uint64 interrupt_ready;
};

extern struct mmix_boot_handoff mmix_boot_handoffs[];
extern struct mmix_startup_control mmix_startup;
extern char kernel_boot_stacks_start[];
extern char kernel_boot_stacks_end[];

static inline uint64
boot_stack_base(uint64 cpu_id)
{
  return (uint64)kernel_boot_stacks_end - (cpu_id + 1) * BOOT_STACK_SIZE;
}

static inline uint64
boot_stack_top(uint64 cpu_id)
{
  return (uint64)kernel_boot_stacks_end - cpu_id * BOOT_STACK_SIZE;
}

void start(uint64 startup_cpu_id, uint64 fdt_address, uint64 entry_rl,
           uint64 entry_ro, uint64 entry_rs)
    __attribute__((noreturn));
int boot_wait_for_online(void);
// Boot CPU only, after discovery/online validation and kinit, before kvminit.
// Returns a physmem_status; the entire copied FDT reservation is released once.
int boot_reclaim_fdt(void);
int boot_publish_interrupt_ready(void);
int boot_wait_for_interrupt_ready(void);
int boot_release_schedulers(void);
int boot_wait_for_scheduler_release(void);
int boot_publish_scheduler_ready(void);

_Static_assert(sizeof(struct mmix_boot_handoff) == 7 * sizeof(uint64),
               "unexpected MMIX boot handoff size");
_Static_assert(__alignof__(struct mmix_startup_control) == sizeof(uint64),
               "startup control must be octa-aligned");
_Static_assert(sizeof(struct mmix_startup_control) ==
                 (10 + 2 * MMIX_MAX_CPUS) * sizeof(uint64),
               "unexpected startup control size");

#endif
