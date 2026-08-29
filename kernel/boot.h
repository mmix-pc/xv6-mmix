#ifndef XV6_MMIX_BOOT_H
#define XV6_MMIX_BOOT_H

#include "types.h"
#include "bootinfo.h"
#include "memlayout.h"

// Immutable entry data retained separately before the SMP startup barrier can
// publish the canonical boot description.
struct mmix_boot_handoff {
  uint64 startup_cpu_id;
  uint64 bootinfo_pa;
  uint64 software_stack_base;
  uint64 software_stack_top;
  uint64 register_stack_base;
  uint64 register_stack_limit;
};

// State retained by the minimal MMIX boot path for early diagnostics.
struct mmix_boot_state {
  uint64 startup_cpu_id;
  uint64 bootinfo_pa;
  int bootinfo_status;
  struct mmix_bootinfo info;
};

enum mmix_startup_state {
  MMIX_STARTUP_RESET = 0,
  MMIX_STARTUP_COLLECTING = 1,
  MMIX_STARTUP_INITIALIZING = 2,
  MMIX_STARTUP_GLOBAL_READY = 3,
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
  MMIX_CPU_STAGE_SERVICE = 9,
  MMIX_CPU_STAGE_SECONDARY_IDLE = 10,
};

enum mmix_startup_failure {
  MMIX_STARTUP_FAILURE_NONE = 0,
  MMIX_STARTUP_FAILURE_BOOTINFO = 1,
  MMIX_STARTUP_FAILURE_DUPLICATE_ARRIVAL = 2,
  MMIX_STARTUP_FAILURE_TOPOLOGY = 3,
  MMIX_STARTUP_FAILURE_GENERATION = 4,
  MMIX_STARTUP_FAILURE_DUPLICATE_INITIALIZER = 5,
  MMIX_STARTUP_FAILURE_PREMATURE_PUBLICATION = 6,
  MMIX_STARTUP_FAILURE_DUPLICATE_ONLINE = 7,
  MMIX_STARTUP_FAILURE_DUPLICATE_CONTEXT = 8,
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
  uint64 cpu_stage[MMIX_MAX_CPUS];
  uint64 context_transfers[MMIX_MAX_CPUS];
};

extern struct mmix_boot_state mmix_boot;
extern struct mmix_boot_handoff mmix_boot_handoffs[];
extern struct mmix_startup_control mmix_startup;

// Valid after bootinfo_status reports a successful decode.
static inline const struct mmix_physical_memory *
boot_physical_memory(void)
{
  return &mmix_boot.info.memory;
}

void start(uint64 startup_cpu_id, uint64 bootinfo_pa)
    __attribute__((noreturn));
int boot_publish_global_ready(void);
int boot_wait_for_online(void);

_Static_assert(sizeof(struct mmix_boot_handoff) == 6 * sizeof(uint64),
               "unexpected MMIX boot handoff size");
_Static_assert(__alignof__(struct mmix_startup_control) == sizeof(uint64),
               "startup control must be octa-aligned");
_Static_assert(sizeof(struct mmix_startup_control) ==
                 (8 + 2 * MMIX_MAX_CPUS) * sizeof(uint64),
               "unexpected startup control size");

#endif
