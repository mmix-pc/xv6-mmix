#ifndef XV6_MMIX_BOOT_H
#define XV6_MMIX_BOOT_H

#include "types.h"
#include "bootinfo.h"

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

extern struct mmix_boot_state mmix_boot;
extern struct mmix_boot_handoff mmix_boot_handoffs[];

// Valid after bootinfo_status reports a successful decode.
static inline const struct mmix_physical_memory *
boot_physical_memory(void)
{
  return &mmix_boot.info.memory;
}

void start(uint64 startup_cpu_id, uint64 bootinfo_pa)
    __attribute__((noreturn));

_Static_assert(sizeof(struct mmix_boot_handoff) == 6 * sizeof(uint64),
               "unexpected MMIX boot handoff size");

#endif
