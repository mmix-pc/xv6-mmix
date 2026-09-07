#ifndef XV6_MMIX_PLATFORM_H
#define XV6_MMIX_PLATFORM_H

#include "param.h"
#include "types.h"

#define INITIAL_REGISTER_STACK_SIZE 0x00008000UL

enum platform_status {
  PLATFORM_OK = 0,
  PLATFORM_BAD_ARGUMENT = -1,
  PLATFORM_BAD_ROOT = -2,
  PLATFORM_BAD_CPU_BUS = -3,
  PLATFORM_BAD_CPU = -4,
  PLATFORM_TOO_MANY_CPUS = -5,
  PLATFORM_BAD_CPU_IDS = -6,
  PLATFORM_BAD_PHANDLE = -7,
  PLATFORM_BAD_MEMORY = -8,
  PLATFORM_BAD_REGISTER_STACK = -9,
  PLATFORM_REGISTER_STACK_OUTSIDE_RAM = -10,
  PLATFORM_REGISTER_STACK_OVERLAP = -11,
};

struct platform_cpu {
  uint32 id;
  uint64 initial_register_stack;
  uint64 initial_register_stack_size;
};

struct platform_cpu_topology {
  // All data is copied from the FDT and indexed by the validated CPU ID.
  uint32 count;
  struct platform_cpu cpus[NCPU];
};

struct fdt;

int platform_decode_cpu_topology(const struct fdt *,
                                 struct platform_cpu_topology *);

#endif
