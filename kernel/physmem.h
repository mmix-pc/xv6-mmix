#ifndef XV6_MMIX_PHYSMEM_H
#define XV6_MMIX_PHYSMEM_H

#include "param.h"
#include "types.h"

#define PHYSMEM_MAX_RESERVATIONS (NCPU + 6)
#define PHYSMEM_MAX_SPANS        (PHYSMEM_MAX_RESERVATIONS + 1)

enum physmem_status {
  PHYSMEM_OK = 0,
  PHYSMEM_BAD_ARGUMENT = -1,
  PHYSMEM_NOT_READY = -2,
  PHYSMEM_ALREADY_INITIALIZED = -3,
  PHYSMEM_BAD_PLATFORM = -4,
  PHYSMEM_BAD_RAM = -5,
  PHYSMEM_BAD_IMAGE = -6,
  PHYSMEM_RANGE_OVERFLOW = -7,
  PHYSMEM_RANGE_OUTSIDE_RAM = -8,
  PHYSMEM_RESERVATION_OVERLAP = -9,
  PHYSMEM_DUPLICATE_RESERVATION = -10,
  PHYSMEM_TOO_MANY_RANGES = -11,
  PHYSMEM_ALREADY_RELEASED = -12,
};

struct physmem_span {
  uint64 physical_base;
  uint64 size;
};

// A release may uncover several spans when another live reservation overlaps
// its page envelope. The returned spans are the only newly eligible pages.
struct physmem_release {
  struct physmem_span spans[PHYSMEM_MAX_SPANS];
  uint32 span_count;
  uint64 page_count;
};

int physmem_init(void);
uint32 physmem_span_count(void);
int physmem_span(uint32, struct physmem_span *);
uint64 physmem_managed_pages(void);
uint64 physmem_reserved_pages(void);
int physmem_release_fdt(struct physmem_release *);
int physmem_release_cpu_stack(uint32, struct physmem_release *);
int physmem_release_boot_stacks(struct physmem_release *);

#endif
