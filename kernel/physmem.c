#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "platform.h"
#include "physmem.h"

extern char kernel_image_start[];
extern char kernel_boot_stacks_start[];
extern char kernel_boot_stacks_end[];
extern char kernel_end[];
extern char kernel_low_vectors_start[];
extern char kernel_low_vectors_end[];
extern char kernel_root_tables_start[];
extern char kernel_root_tables_end[];

enum reservation_kind {
  RESERVATION_LOW_VECTORS,
  RESERVATION_ROOT_TABLES,
  RESERVATION_KERNEL_IMAGE,
  RESERVATION_BOOT_STACKS,
  RESERVATION_FDT,
  RESERVATION_CPU_STACK,
  RESERVATION_FRAMEBUFFER,
};

struct reservation {
  uint64 start;
  uint64 limit;
  enum reservation_kind kind;
  uint32 cpu_id;
  uint8 permanent;
  uint8 live;
};

struct physmem_inputs {
  struct platform_physical_range ram;
  struct platform_reservation_info platform[PLATFORM_MAX_RESERVATIONS];
  uint32 platform_count;
  uint32 cpu_count;
  uint64 image_start;
  uint64 image_permanent_limit;
  uint64 image_limit;
  uint64 boot_stacks_start;
  uint64 boot_stacks_limit;
  uint64 vector_start;
  uint64 vector_limit;
  uint64 root_start;
  uint64 root_limit;
};

struct physmem_plan {
  struct reservation reservations[PHYSMEM_MAX_RESERVATIONS];
  struct physmem_span spans[PHYSMEM_MAX_SPANS];
  uint32 reservation_count;
  uint32 span_count;
  uint32 cpu_count;
  uint64 physical_pages;
  uint64 managed_pages;
  uint8 ready;
};

static struct physmem_plan plan;
static volatile uint32 plan_lock;

static int
ranges_overlap(uint64 left_start, uint64 left_limit, uint64 right_start,
               uint64 right_limit)
{
  return left_start < right_limit && right_start < left_limit;
}

static void
lock_plan(void)
{
  while (__atomic_exchange_n(&plan_lock, 1, __ATOMIC_ACQUIRE) != 0)
    asm volatile("SWYM 0,0,0" ::: "memory");
}

static void
unlock_plan(void)
{
  __atomic_store_n(&plan_lock, 0, __ATOMIC_RELEASE);
}

static int
page_envelope(uint64 start, uint64 size, uint64 *page_start, uint64 *page_limit)
{
  uint64 limit;

  if (size == 0)
    return PHYSMEM_BAD_ARGUMENT;
  if (size > ~start)
    return PHYSMEM_RANGE_OVERFLOW;
  limit = start + size;
  if (limit > ~(MMIX_PAGE_SIZE - 1))
    return PHYSMEM_RANGE_OVERFLOW;
  *page_start = ROUNDDOWN(start, MMIX_PAGE_SIZE);
  *page_limit = ROUNDUP(limit, MMIX_PAGE_SIZE);
  return PHYSMEM_OK;
}

static int
add_reservation(struct physmem_plan *candidate, uint64 start, uint64 limit,
                enum reservation_kind kind, uint32 cpu_id, int permanent)
{
  struct reservation *reservation;

  if (start >= limit || (start & (MMIX_PAGE_SIZE - 1)) != 0 ||
      (limit & (MMIX_PAGE_SIZE - 1)) != 0)
    return PHYSMEM_BAD_ARGUMENT;
  if (candidate->reservation_count == PHYSMEM_MAX_RESERVATIONS)
    return PHYSMEM_TOO_MANY_RANGES;
  reservation = &candidate->reservations[candidate->reservation_count++];
  *reservation = (struct reservation){
    .start = start,
    .limit = limit,
    .kind = kind,
    .cpu_id = cpu_id,
    .permanent = permanent != 0,
    .live = 1,
  };
  return PHYSMEM_OK;
}

static int
add_free_span(struct physmem_plan *candidate, uint64 start, uint64 limit)
{
  if (start == limit)
    return PHYSMEM_OK;
  if (start > limit || candidate->span_count == PHYSMEM_MAX_SPANS)
    return PHYSMEM_TOO_MANY_RANGES;
  candidate->spans[candidate->span_count++] = (struct physmem_span){
    .physical_base = start,
    .size = limit - start,
  };
  candidate->managed_pages += (limit - start) / MMIX_PAGE_SIZE;
  return PHYSMEM_OK;
}

static int
rebuild_spans(struct physmem_plan *candidate)
{
  struct physmem_span excluded[PHYSMEM_MAX_RESERVATIONS];
  uint32 excluded_count = 0;
  uint64 cursor = 0;

  candidate->span_count = 0;
  candidate->managed_pages = 0;
  for (uint32 i = 0; i < candidate->reservation_count; i++) {
    const struct reservation *reservation = &candidate->reservations[i];

    if (!reservation->live)
      continue;
    excluded[excluded_count++] = (struct physmem_span){
      .physical_base = reservation->start,
      .size = reservation->limit - reservation->start,
    };
  }
  for (uint32 i = 1; i < excluded_count; i++) {
    struct physmem_span value = excluded[i];
    uint32 position = i;

    while (position != 0 &&
           excluded[position - 1].physical_base > value.physical_base) {
      excluded[position] = excluded[position - 1];
      position--;
    }
    excluded[position] = value;
  }
  for (uint32 i = 0; i < excluded_count; i++) {
    uint64 start = excluded[i].physical_base;
    uint64 limit = start + excluded[i].size;
    int status;

    if (start > cursor) {
      status = add_free_span(candidate, cursor, start);
      if (status != PHYSMEM_OK)
        return status;
    }
    if (limit > cursor)
      cursor = limit;
  }
  return add_free_span(candidate, cursor,
                       candidate->physical_pages * MMIX_PAGE_SIZE);
}

static int
validate_static_inputs(const struct physmem_inputs *inputs)
{
  int supported_ram = inputs->ram.size == 128UL * 1024 * 1024 ||
                      inputs->ram.size == 256UL * 1024 * 1024 ||
                      inputs->ram.size == 512UL * 1024 * 1024 ||
                      inputs->ram.size == 640UL * 1024 * 1024 ||
                      inputs->ram.size == 1024UL * 1024 * 1024;

  if (inputs->ram.physical_base != 0 || inputs->ram.size == 0 ||
      (inputs->ram.size & (MMIX_PAGE_SIZE - 1)) != 0 ||
      inputs->ram.size < PLATFORM_RAM_MIN_SIZE ||
      inputs->ram.size > PLATFORM_RAM_MAX_SIZE || !supported_ram)
    return PHYSMEM_BAD_RAM;
  if (inputs->vector_start != 0 ||
      inputs->vector_limit - inputs->vector_start != MMIX_PAGE_SIZE ||
      inputs->root_start != inputs->vector_limit ||
      inputs->root_limit - inputs->root_start !=
        KERNEL_ROOT_BLOCKS * MMIX_PAGE_SIZE ||
      inputs->image_start != KERNEL_LOAD ||
      inputs->root_limit > inputs->image_start ||
      inputs->image_permanent_limit != inputs->boot_stacks_start ||
      inputs->boot_stacks_limit != inputs->image_limit ||
      inputs->boot_stacks_limit - inputs->boot_stacks_start !=
        BOOT_STACK_AREA_SIZE)
    return PHYSMEM_BAD_IMAGE;
  if ((inputs->vector_start | inputs->vector_limit | inputs->root_start |
       inputs->root_limit | inputs->image_start |
       inputs->image_permanent_limit | inputs->boot_stacks_start |
       inputs->boot_stacks_limit | inputs->image_limit) &
      (MMIX_PAGE_SIZE - 1))
    return PHYSMEM_BAD_IMAGE;
  if (inputs->image_start >= inputs->image_permanent_limit ||
      inputs->boot_stacks_start >= inputs->boot_stacks_limit ||
      inputs->boot_stacks_limit > inputs->ram.size)
    return PHYSMEM_RANGE_OUTSIDE_RAM;
  return PHYSMEM_OK;
}

static int
build_plan(const struct physmem_inputs *inputs, struct physmem_plan *candidate)
{
  uint64 cpu_mask = 0;
  uint32 fdt_count = 0;
  uint32 framebuffer_count = 0;
  int status;

  if (inputs == 0 || candidate == 0)
    return PHYSMEM_BAD_ARGUMENT;
  *candidate = (struct physmem_plan){0};
  status = validate_static_inputs(inputs);
  if (status != PHYSMEM_OK)
    return status;
  if (inputs->platform_count > PLATFORM_MAX_RESERVATIONS)
    return PHYSMEM_TOO_MANY_RANGES;
  if (inputs->cpu_count == 0 || inputs->cpu_count > NCPU ||
      inputs->platform_count != inputs->cpu_count + 2)
    return PHYSMEM_BAD_PLATFORM;
  candidate->physical_pages = inputs->ram.size / MMIX_PAGE_SIZE;
  candidate->cpu_count = inputs->cpu_count;

  status =
    add_reservation(candidate, inputs->vector_start, inputs->vector_limit,
                    RESERVATION_LOW_VECTORS, PLATFORM_NO_CPU, 1);
  if (status == PHYSMEM_OK)
    status = add_reservation(candidate, inputs->root_start, inputs->root_limit,
                             RESERVATION_ROOT_TABLES, PLATFORM_NO_CPU, 1);
  if (status == PHYSMEM_OK)
    status = add_reservation(candidate, inputs->image_start,
                             inputs->image_permanent_limit,
                             RESERVATION_KERNEL_IMAGE, PLATFORM_NO_CPU, 1);
  if (status == PHYSMEM_OK)
    status = add_reservation(candidate, inputs->boot_stacks_start,
                             inputs->boot_stacks_limit, RESERVATION_BOOT_STACKS,
                             PLATFORM_NO_CPU, 0);
  if (status != PHYSMEM_OK)
    return status;

  for (uint32 i = 0; i < inputs->platform_count; i++) {
    const struct platform_reservation_info *source = &inputs->platform[i];
    enum reservation_kind kind;
    uint64 start;
    uint64 limit;
    int permanent = 0;

    status = page_envelope(source->physical.physical_base,
                           source->physical.size, &start, &limit);
    if (status != PHYSMEM_OK)
      return status;
    if (limit > inputs->ram.size)
      return PHYSMEM_RANGE_OUTSIDE_RAM;
    if (source->owner == PLATFORM_RESERVATION_FDT &&
        source->lifetime == PLATFORM_RESERVATION_UNTIL_PLATFORM_COPIED &&
        source->cpu_id == PLATFORM_NO_CPU) {
      kind = RESERVATION_FDT;
      fdt_count++;
    } else if (source->owner == PLATFORM_RESERVATION_CPU_REGISTER_STACK &&
               source->lifetime == PLATFORM_RESERVATION_UNTIL_CPU_RELEASED &&
               source->cpu_id < inputs->cpu_count &&
               source->physical.size == INITIAL_REGISTER_STACK_SIZE &&
               (source->physical.physical_base & (MMIX_PAGE_SIZE - 1)) == 0) {
      kind = RESERVATION_CPU_STACK;
      if ((cpu_mask & (1ULL << source->cpu_id)) != 0)
        return PHYSMEM_DUPLICATE_RESERVATION;
      cpu_mask |= 1ULL << source->cpu_id;
    } else if (source->owner == PLATFORM_RESERVATION_FRAMEBUFFER &&
               source->lifetime == PLATFORM_RESERVATION_DEVICE_LIFETIME &&
               source->cpu_id == PLATFORM_NO_CPU &&
               source->physical.size == PLATFORM_FRAMEBUFFER_SIZE &&
               (source->physical.physical_base & (MMIX_PAGE_SIZE - 1)) == 0) {
      kind = RESERVATION_FRAMEBUFFER;
      permanent = 1;
      framebuffer_count++;
    } else {
      return PHYSMEM_BAD_PLATFORM;
    }
    if (fdt_count > 1 || framebuffer_count > 1)
      return PHYSMEM_DUPLICATE_RESERVATION;
    for (uint32 j = 0; j < 4; j++) {
      const struct reservation *other = &candidate->reservations[j];

      if (ranges_overlap(start, limit, other->start, other->limit))
        return PHYSMEM_RESERVATION_OVERLAP;
    }
    for (uint32 j = 0; j < i; j++) {
      const struct platform_reservation_info *other = &inputs->platform[j];
      uint64 other_limit;

      if (other->physical.size > ~other->physical.physical_base)
        return PHYSMEM_RANGE_OVERFLOW;
      other_limit = other->physical.physical_base + other->physical.size;
      if (ranges_overlap(source->physical.physical_base,
                         source->physical.physical_base + source->physical.size,
                         other->physical.physical_base, other_limit))
        return PHYSMEM_RESERVATION_OVERLAP;
    }
    for (uint32 j = 4; j < candidate->reservation_count; j++) {
      const struct reservation *other = &candidate->reservations[j];

      if ((permanent || other->permanent) &&
          ranges_overlap(start, limit, other->start, other->limit))
        return PHYSMEM_RESERVATION_OVERLAP;
    }
    status =
      add_reservation(candidate, start, limit, kind, source->cpu_id, permanent);
    if (status != PHYSMEM_OK)
      return status;
  }
  if (fdt_count != 1 || framebuffer_count != 1 ||
      cpu_mask != (1ULL << inputs->cpu_count) - 1)
    return PHYSMEM_BAD_PLATFORM;
  status = rebuild_spans(candidate);
  if (status != PHYSMEM_OK)
    return status;
  candidate->ready = 1;
  return PHYSMEM_OK;
}

static int
load_inputs(struct physmem_inputs *inputs)
{
  uint32 count;

  *inputs = (struct physmem_inputs){
    .cpu_count = platform_cpu_count(),
    .image_start = (uint64)kernel_image_start,
    .image_permanent_limit = (uint64)kernel_boot_stacks_start,
    .image_limit = (uint64)kernel_end,
    .boot_stacks_start = (uint64)kernel_boot_stacks_start,
    .boot_stacks_limit = (uint64)kernel_boot_stacks_end,
    .vector_start = (uint64)kernel_low_vectors_start,
    .vector_limit = (uint64)kernel_low_vectors_end,
    .root_start = (uint64)kernel_root_tables_start,
    .root_limit = (uint64)kernel_root_tables_end,
  };
  if (platform_ram(&inputs->ram) != PLATFORM_OK)
    return PHYSMEM_BAD_PLATFORM;
  count = platform_reservation_count();
  if (count > PLATFORM_MAX_RESERVATIONS)
    return PHYSMEM_TOO_MANY_RANGES;
  inputs->platform_count = count;
  for (uint32 i = 0; i < count; i++) {
    if (platform_reservation(i, &inputs->platform[i]) != PLATFORM_OK)
      return PHYSMEM_BAD_PLATFORM;
  }
  return PHYSMEM_OK;
}

int
physmem_init(void)
{
  struct physmem_inputs inputs;
  struct physmem_plan candidate;
  int status = load_inputs(&inputs);

  if (status == PHYSMEM_OK)
    status = build_plan(&inputs, &candidate);
  if (status != PHYSMEM_OK)
    return status;
  lock_plan();
  if (plan.ready) {
    unlock_plan();
    return PHYSMEM_ALREADY_INITIALIZED;
  }
  plan = candidate;
  unlock_plan();
  return PHYSMEM_OK;
}

uint32
physmem_span_count(void)
{
  uint32 count;

  lock_plan();
  count = plan.ready ? plan.span_count : 0;
  unlock_plan();
  return count;
}

int
physmem_span(uint32 index, struct physmem_span *span)
{
  int status = PHYSMEM_OK;

  if (span == 0)
    return PHYSMEM_BAD_ARGUMENT;
  lock_plan();
  if (!plan.ready)
    status = PHYSMEM_NOT_READY;
  else if (index >= plan.span_count)
    status = PHYSMEM_BAD_ARGUMENT;
  else
    *span = plan.spans[index];
  unlock_plan();
  return status;
}

uint64
physmem_managed_pages(void)
{
  uint64 pages;

  lock_plan();
  pages = plan.ready ? plan.managed_pages : 0;
  unlock_plan();
  return pages;
}

uint64
physmem_reserved_pages(void)
{
  uint64 pages;

  lock_plan();
  pages = plan.ready ? plan.physical_pages - plan.managed_pages : 0;
  unlock_plan();
  return pages;
}

static int
release_reservation(enum reservation_kind kind, uint32 cpu_id,
                    struct physmem_release *released)
{
  struct physmem_plan candidate;
  struct reservation *target = 0;
  uint64 target_start = 0;
  uint64 target_limit = 0;
  int status;

  if (released == 0)
    return PHYSMEM_BAD_ARGUMENT;
  *released = (struct physmem_release){0};
  lock_plan();
  if (!plan.ready) {
    unlock_plan();
    return PHYSMEM_NOT_READY;
  }
  candidate = plan;
  for (uint32 i = 0; i < candidate.reservation_count; i++) {
    struct reservation *reservation = &candidate.reservations[i];

    if (reservation->kind == kind && reservation->cpu_id == cpu_id) {
      target = reservation;
      break;
    }
  }
  if (target == 0) {
    unlock_plan();
    return PHYSMEM_BAD_ARGUMENT;
  }
  if (!target->live) {
    unlock_plan();
    return PHYSMEM_ALREADY_RELEASED;
  }
  if (target->permanent) {
    unlock_plan();
    return PHYSMEM_BAD_ARGUMENT;
  }
  target_start = target->start;
  target_limit = target->limit;
  target->live = 0;
  status = rebuild_spans(&candidate);
  if (status != PHYSMEM_OK) {
    unlock_plan();
    return status;
  }
  for (uint32 i = 0; i < candidate.span_count; i++) {
    uint64 start = candidate.spans[i].physical_base;
    uint64 limit = start + candidate.spans[i].size;

    if (start < target_start)
      start = target_start;
    if (limit > target_limit)
      limit = target_limit;
    if (start >= limit)
      continue;
    released->spans[released->span_count++] = (struct physmem_span){
      .physical_base = start,
      .size = limit - start,
    };
    released->page_count += (limit - start) / MMIX_PAGE_SIZE;
  }
  plan = candidate;
  unlock_plan();
  return PHYSMEM_OK;
}

int
physmem_release_fdt(struct physmem_release *released)
{
  return release_reservation(RESERVATION_FDT, PLATFORM_NO_CPU, released);
}

int
physmem_release_cpu_stack(uint32 cpu_id, struct physmem_release *released)
{
  if (cpu_id >= NCPU)
    return PHYSMEM_BAD_ARGUMENT;
  return release_reservation(RESERVATION_CPU_STACK, cpu_id, released);
}

int
physmem_release_boot_stacks(struct physmem_release *released)
{
  return release_reservation(RESERVATION_BOOT_STACKS, PLATFORM_NO_CPU,
                             released);
}

_Static_assert(PHYSMEM_MAX_RESERVATIONS == PLATFORM_MAX_RESERVATIONS + 4,
               "planner capacity must include linker reservations");
