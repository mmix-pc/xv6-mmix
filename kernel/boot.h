#ifndef XV6_MMIX_BOOT_H
#define XV6_MMIX_BOOT_H

#include "types.h"
#include "bootinfo.h"

// State retained by the minimal MMIX boot path for early diagnostics.
struct mmix_boot_state {
  uint64 startup_cpu_id;
  uint64 bootinfo_pa;
  int bootinfo_status;
  struct mmix_bootinfo info;
};

extern struct mmix_boot_state mmix_boot;

// Valid after bootinfo_status reports a successful decode.
static inline const struct mmix_physical_memory *
boot_physical_memory(void)
{
  return &mmix_boot.info.memory;
}

void start(uint64 startup_cpu_id, uint64 bootinfo_pa)
    __attribute__((noreturn));

#endif
