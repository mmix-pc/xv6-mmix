// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 8192-byte MMIX pages.

#include "mmix.h"
#include "param.h"
#include "boot.h"
#include "kalloc.h"
#include "spinlock.h"
#include "defs.h"

extern char kernel_end[]; // First address after the loaded kernel.
                          // Defined by kernel.ld.

struct run {
  struct run *next;
};

struct kalloc_zone {
  struct run *freelist;
  uint64 free_pages;
  uint64 managed_pages;
  int ready;
};

enum {
  KALLOC_LOW_ZONE,
  KALLOC_RECLAIMED_ZONE,
  KALLOC_HIGH_ZONE,
  KALLOC_ZONE_COUNT,
  KALLOC_EAGER_BUDGET_PAGES = 100 * 1024 * 1024 / PGSIZE,
};

static struct {
  struct spinlock lock;
  struct kalloc_zone zone[KALLOC_ZONE_COUNT];
} kmem;

static uint64
high_ram_size(void)
{
  return mmix_platform.memory.ram_size > PHYSICAL_LOW_RAM_SIZE
           ? mmix_platform.memory.ram_size - PHYSICAL_LOW_RAM_SIZE
           : 0;
}

_Static_assert((100 * 1024 * 1024) % PGSIZE == 0,
               "eager allocation budget must use whole pages");
_Static_assert(KALLOC_RECLAIMED_PAGES >= KALLOC_EAGER_BUDGET_PAGES,
               "minimum general memory must contain the eager budget");

static int
zone_bounds(int zone, uint64 *start, uint64 *limit)
{
  if (zone == KALLOC_LOW_ZONE) {
    *start = KALLOC_START((uint64)kernel_end);
    *limit = KALLOC_LOW_LIMIT;
  } else if (zone == KALLOC_RECLAIMED_ZONE) {
    *start = KALLOC_RECLAIMED_START;
    *limit = KALLOC_RECLAIMED_LIMIT;
  } else if (zone == KALLOC_HIGH_ZONE) {
    *start = PHYSICAL_HIGH_RAM_BASE;
    *limit = PHYSICAL_HIGH_RAM_BASE + high_ram_size();
  } else {
    return -1;
  }
  return 0;
}

static int
page_zone(uint64 address)
{
  if ((address & (PGSIZE - 1)) != 0)
    return -1;
  for (int zone = 0; zone < KALLOC_ZONE_COUNT; zone++) {
    uint64 start;
    uint64 limit;

    if (zone_bounds(zone, &start, &limit) < 0)
      return -1;
    if (address >= start && address < limit)
      return zone;
  }
  return -1;
}

static void
allocator_audit_locked(void)
{
  uint64 managed = 0;

  for (int zone = 0; zone < KALLOC_ZONE_COUNT; zone++) {
    struct kalloc_zone *current = &kmem.zone[zone];
    struct run *run = current->freelist;
    uint64 start;
    uint64 limit;
    uint64 count = 0;

    if (zone_bounds(zone, &start, &limit) < 0 ||
        (!current->ready &&
         (run != 0 || current->free_pages != 0 ||
          current->managed_pages != 0)))
      panic("kalloc audit");
    if (!current->ready)
      continue;
    if (current->managed_pages != (limit - start) / PGSIZE ||
        current->free_pages > current->managed_pages)
      panic("kalloc audit");
    while (run != 0 && count <= current->free_pages) {
      if (page_zone((uint64)run) != zone)
        panic("kalloc audit");
      run = run->next;
      count++;
    }
    if (count != current->free_pages)
      panic("kalloc audit");
    managed += current->managed_pages;
  }

  if (kmem.zone[KALLOC_LOW_ZONE].ready &&
      kmem.zone[KALLOC_RECLAIMED_ZONE].ready &&
      kmem.zone[KALLOC_HIGH_ZONE].ready) {
    uint64 first = KALLOC_START((uint64)kernel_end);
    uint64 reserved = first / PGSIZE +
                      (LOW_RAM_END - KALLOC_LOW_LIMIT) / PGSIZE +
                      (PHYSICAL_LOW_RAM_END - KALLOC_RECLAIMED_LIMIT) /
                        PGSIZE;
    uint64 physical = mmix_platform.memory.ram_size / PGSIZE;

    if (managed > physical || physical - managed != reserved)
      panic("kalloc topology");
  }
}

static void
publish_zone(int zone, uint64 start, uint64 limit, char *panic_message,
             char *repeat_message)
{
  uint64 expected;
  uint64 zone_start;
  uint64 zone_limit;

  if (zone_bounds(zone, &zone_start, &zone_limit) < 0 ||
      start != zone_start || limit != zone_limit || start > limit ||
      (start & (PGSIZE - 1)) != 0 ||
      (limit & (PGSIZE - 1)) != 0 ||
      (start != limit &&
       (page_zone(start) != zone || page_zone(limit - PGSIZE) != zone)))
    panic(panic_message);
  expected = (limit - start) / PGSIZE;

  acquire(&kmem.lock);
  if (kmem.zone[zone].ready || kmem.zone[zone].freelist != 0 ||
      kmem.zone[zone].free_pages != 0) {
    release(&kmem.lock);
    panic(repeat_message);
  }

  if (start == limit) {
    kmem.zone[zone].ready = 1;
    allocator_audit_locked();
    release(&kmem.lock);
    return;
  }

  // Newly published pages have no dangling references. Link them directly
  // instead of eagerly filling the whole zone during boot.
  for (char *page = (char *)start; page < (char *)limit; page += PGSIZE) {
    struct run *r = (struct run *)page;

    r->next = kmem.zone[zone].freelist;
    kmem.zone[zone].freelist = r;
    kmem.zone[zone].free_pages++;
  }
  if (kmem.zone[zone].free_pages != expected) {
    release(&kmem.lock);
    panic(panic_message);
  }
  kmem.zone[zone].managed_pages = expected;
  kmem.zone[zone].ready = 1;
  allocator_audit_locked();
  release(&kmem.lock);
}

void
kinit()
{
  uint64 first = KALLOC_START((uint64)kernel_end);

  if ((first & (PGSIZE - 1)) != 0 || first < KERNEL_LOAD ||
      first >= KALLOC_LOW_LIMIT)
    panic("kinit");

  initlock(&kmem.lock, "kmem");
  publish_zone(KALLOC_LOW_ZONE, first, KALLOC_LOW_LIMIT, "kinit",
               "kinit twice");
}

void
kinit_reclaimed(void)
{
  if (mmix_rv_read() != MMIX_KERNEL_RV)
    panic("kinit reclaimed");

  publish_zone(KALLOC_RECLAIMED_ZONE, KALLOC_RECLAIMED_START,
               KALLOC_RECLAIMED_LIMIT, "kinit reclaimed",
               "kinit reclaimed twice");
}

void
kinit_high(void)
{
  uint64 high_size = high_ram_size();

  if (mmix_rv_read() != MMIX_KERNEL_RV)
    panic("kinit high");
  publish_zone(KALLOC_HIGH_ZONE, PHYSICAL_HIGH_RAM_BASE,
               PHYSICAL_HIGH_RAM_BASE + high_size,
               "kinit high", "kinit high twice");
}

int
kalloc_page_is_managed(void *pa)
{
  int managed;
  int zone = page_zone((uint64)pa);

  acquire(&kmem.lock);
  managed = zone >= 0 && kmem.zone[zone].ready;
  release(&kmem.lock);
  return managed;
}

int
kalloc_page_is_dma(void *pa)
{
  return page_zone((uint64)pa) == KALLOC_LOW_ZONE;
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing an allocator zone.)
void
kfree(void *pa)
{
  struct run *r;
  int zone = page_zone((uint64)pa);

  if (zone < 0)
    panic("kfree");

  acquire(&kmem.lock);
  if (!kmem.zone[zone].ready) {
    release(&kmem.lock);
    panic("kfree unpublished");
  }
  release(&kmem.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run *)pa;

  acquire(&kmem.lock);
  if (kmem.zone[zone].free_pages >= kmem.zone[zone].managed_pages) {
    release(&kmem.lock);
    panic("kfree count");
  }
  r->next = kmem.zone[zone].freelist;
  kmem.zone[zone].freelist = r;
  kmem.zone[zone].free_pages++;
  release(&kmem.lock);
}

static struct run *
alloc_from_zone(struct kalloc_zone *zone)
{
  struct run *r = zone->freelist;

  if ((r == 0) != (zone->free_pages == 0))
    panic("kalloc freelist");
  if (r != 0) {
    zone->freelist = r->next;
    zone->free_pages--;
  }
  return r;
}

static void *
kalloc_from(int dma_only)
{
  struct run *r;

  acquire(&kmem.lock);
  if (!dma_only && kmem.zone[KALLOC_HIGH_ZONE].ready)
    r = alloc_from_zone(&kmem.zone[KALLOC_HIGH_ZONE]);
  else
    r = 0;
  if (r == 0 && !dma_only && kmem.zone[KALLOC_RECLAIMED_ZONE].ready)
    r = alloc_from_zone(&kmem.zone[KALLOC_RECLAIMED_ZONE]);
  if (r == 0)
    r = alloc_from_zone(&kmem.zone[KALLOC_LOW_ZONE]);
  release(&kmem.lock);

  if (r != 0)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

// Allocate one 8192-byte MMIX physical page. Prefer High RAM, then reclaimed
// bare-segment backing, then Low RAM.
void *
kalloc(void)
{
  return kalloc_from(0);
}

// Allocate a page that the current VirtIO device can address.
void *
kalloc_dma(void)
{
  return kalloc_from(1);
}

static void *
alloc_contiguous_from_zone(struct kalloc_zone *zone, uint64 start,
                           uint64 limit, uint count)
{
  struct run *base;

  if (count == 0 || count > (limit - start) / PGSIZE)
    return 0;

  for (base = zone->freelist; base != 0; base = base->next) {
    uint64 address = (uint64)base;
    uint found = 1;

    if (address < start || address > limit - (uint64)count * PGSIZE)
      continue;
    for (uint page = 1; page < count; page++) {
      struct run *candidate;
      uint64 wanted = address + (uint64)page * PGSIZE;

      for (candidate = zone->freelist; candidate != 0;
           candidate = candidate->next)
        if ((uint64)candidate == wanted)
          break;
      if (candidate == 0) {
        found = 0;
        break;
      }
    }
    if (!found)
      continue;

    for (uint page = 0; page < count; page++) {
      struct run **link = &zone->freelist;
      uint64 wanted = address + (uint64)page * PGSIZE;

      while ((uint64)*link != wanted)
        link = &(*link)->next;
      *link = (*link)->next;
    }
    zone->free_pages -= count;
    return (void *)address;
  }
  return 0;
}

// Allocate count physically contiguous pages. This is needed for MMIX rV
// root blocks, which hardware addresses as one contiguous array. A run never
// crosses an allocator-zone boundary.
void *
kalloc_contiguous(uint count)
{
  void *base = 0;

  if (count == 0)
    return 0;

  acquire(&kmem.lock);
  if (kmem.zone[KALLOC_HIGH_ZONE].ready) {
    uint64 high_start;
    uint64 high_limit;

    zone_bounds(KALLOC_HIGH_ZONE, &high_start, &high_limit);
    base = alloc_contiguous_from_zone(&kmem.zone[KALLOC_HIGH_ZONE],
                                      high_start, high_limit, count);
  }
  if (base == 0 && kmem.zone[KALLOC_RECLAIMED_ZONE].ready)
    base = alloc_contiguous_from_zone(&kmem.zone[KALLOC_RECLAIMED_ZONE],
                                      STACK_PHYS_BASE, STACK_PHYS_END,
                                      count);
  if (base == 0 && kmem.zone[KALLOC_RECLAIMED_ZONE].ready)
    base = alloc_contiguous_from_zone(&kmem.zone[KALLOC_RECLAIMED_ZONE],
                                      DATA_PHYS_BASE, DATA_PHYS_END,
                                      count);
  if (base == 0 && kmem.zone[KALLOC_RECLAIMED_ZONE].ready)
    base = alloc_contiguous_from_zone(&kmem.zone[KALLOC_RECLAIMED_ZONE],
                                      POOL_PHYS_BASE, POOL_PHYS_END,
                                      count);
  if (base == 0)
    base = alloc_contiguous_from_zone(&kmem.zone[KALLOC_LOW_ZONE],
                                      KALLOC_START((uint64)kernel_end),
                                      KALLOC_LOW_LIMIT, count);
  release(&kmem.lock);

  if (base != 0)
    memset(base, 5, (uint64)count * PGSIZE);
  return base;
}

uint64
kalloc_free_pages(void)
{
  uint64 count;

  acquire(&kmem.lock);
  count = kmem.zone[KALLOC_LOW_ZONE].free_pages +
          kmem.zone[KALLOC_RECLAIMED_ZONE].free_pages +
          kmem.zone[KALLOC_HIGH_ZONE].free_pages;
  release(&kmem.lock);
  return count;
}

void
kalloc_get_stats(struct kalloc_stats *stats)
{
  if (stats == 0)
    panic("kalloc stats");

  acquire(&kmem.lock);
  allocator_audit_locked();
  stats->physical_pages = mmix_platform.memory.ram_size / PGSIZE;
  stats->managed_pages = 0;
  stats->free_pages = 0;
  for (int zone = 0; zone < KALLOC_ZONE_COUNT; zone++) {
    stats->managed_pages += kmem.zone[zone].managed_pages;
    stats->free_pages += kmem.zone[zone].free_pages;
  }
  stats->high_managed_pages = kmem.zone[KALLOC_HIGH_ZONE].managed_pages;
  stats->high_free_pages = kmem.zone[KALLOC_HIGH_ZONE].free_pages;
  release(&kmem.lock);
}
