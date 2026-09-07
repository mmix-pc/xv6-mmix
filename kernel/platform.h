#ifndef XV6_MMIX_PLATFORM_H
#define XV6_MMIX_PLATFORM_H

#include "param.h"
#include "types.h"

#define INITIAL_REGISTER_STACK_SIZE 0x00008000UL
#define PLATFORM_RAM_MIN_SIZE       0x08000000UL
#define PLATFORM_RAM_MAX_SIZE       0x40000000UL
#define PLATFORM_FRAMEBUFFER_SIZE   0x00300000UL
#define PLATFORM_MAX_RESERVATIONS   (NCPU + 2)
#define PLATFORM_NO_CPU             (~0U)

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
  PLATFORM_BAD_RESERVED_MEMORY = -12,
  PLATFORM_BAD_FDT_RESERVATION = -13,
  PLATFORM_BAD_FRAMEBUFFER_MEMORY = -14,
  PLATFORM_RESERVATION_OVERLAP = -15,
  PLATFORM_UNSUPPORTED_INITRD = -16,
};

enum platform_reservation_owner {
  PLATFORM_RESERVATION_FDT,
  PLATFORM_RESERVATION_CPU_REGISTER_STACK,
  PLATFORM_RESERVATION_FRAMEBUFFER,
};

enum platform_reservation_lifetime {
  PLATFORM_RESERVATION_UNTIL_PLATFORM_COPIED,
  PLATFORM_RESERVATION_UNTIL_CPU_RELEASED,
  PLATFORM_RESERVATION_DEVICE_LIFETIME,
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

struct platform_reservation {
  uint64 start;
  uint64 size;
  enum platform_reservation_owner owner;
  enum platform_reservation_lifetime lifetime;
  uint32 cpu_id;
};

struct platform_memory {
  uint64 ram_start;
  uint64 ram_size;
  uint32 reservation_count;
  struct platform_reservation reservations[PLATFORM_MAX_RESERVATIONS];
};

struct platform {
  struct platform_cpu_topology topology;
  struct platform_memory memory;
};

struct fdt;

int platform_decode_cpu_topology(const struct fdt *,
                                 struct platform_cpu_topology *);
int platform_decode(const struct fdt *, uint64, struct platform *);

#endif
