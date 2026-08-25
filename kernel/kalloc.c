// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 8192-byte MMIX pages.

#include "mmix.h"
#include "param.h"
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
};

enum {
  KALLOC_LOW_ZONE,
  KALLOC_RECLAIMED_ZONE,
  KALLOC_EXTENDED_ZONE,
  KALLOC_ZONE_COUNT,
  KALLOC_EAGER_BUDGET_PAGES = 100 * 1024 * 1024 / PGSIZE,
};

static struct {
  struct spinlock lock;
  struct kalloc_zone zone[KALLOC_ZONE_COUNT];
  int reclaimed_ready;
  int extended_ready;
} kmem;

_Static_assert((100 * 1024 * 1024) % PGSIZE == 0,
               "eager allocation budget must use whole pages");
_Static_assert((KALLOC_EXTENDED_LIMIT - KALLOC_EXTENDED_START) / PGSIZE >=
                 KALLOC_EAGER_BUDGET_PAGES,
               "extended RAM must contain the eager allocation budget");

static int
page_zone(uint64 address)
{
  uint64 low_start = KALLOC_START((uint64)kernel_end);

  if ((address & (PGSIZE - 1)) != 0)
    return -1;
  if (address >= low_start && address < KALLOC_LOW_LIMIT)
    return KALLOC_LOW_ZONE;
  if (address >= KALLOC_RECLAIMED_START &&
      address < KALLOC_RECLAIMED_LIMIT)
    return KALLOC_RECLAIMED_ZONE;
  if (address >= KALLOC_EXTENDED_START && address < KALLOC_EXTENDED_LIMIT)
    return KALLOC_EXTENDED_ZONE;
  return -1;
}

static void
publish_zone(int zone, uint64 start, uint64 limit, int *ready,
             char *panic_message, char *repeat_message)
{
  uint64 expected = (limit - start) / PGSIZE;

  if (start >= limit || (start & (PGSIZE - 1)) != 0 ||
      (limit & (PGSIZE - 1)) != 0 || page_zone(start) != zone ||
      page_zone(limit - PGSIZE) != zone)
    panic(panic_message);

  acquire(&kmem.lock);
  if (*ready || kmem.zone[zone].freelist != 0 ||
      kmem.zone[zone].free_pages != 0) {
    release(&kmem.lock);
    panic(repeat_message);
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
  *ready = 1;
  release(&kmem.lock);
}

static void
freerange(void *pa_start, void *pa_end)
{
  char *p = (char *)PGROUNDUP((uint64)pa_start);

  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

void
kinit()
{
  uint64 first = KALLOC_START((uint64)kernel_end);

  if ((first & (PGSIZE - 1)) != 0 || first < KERNEL_LOAD ||
      first >= KALLOC_LOW_LIMIT)
    panic("kinit");

  initlock(&kmem.lock, "kmem");
  freerange((void *)first, (void *)KALLOC_LOW_LIMIT);
}

void
kinit_reclaimed(void)
{
  if (mmix_rv_read() != MMIX_KERNEL_RV)
    panic("kinit reclaimed");

  publish_zone(KALLOC_RECLAIMED_ZONE, KALLOC_RECLAIMED_START,
               KALLOC_RECLAIMED_LIMIT, &kmem.reclaimed_ready,
               "kinit reclaimed", "kinit reclaimed twice");
}

void
kinit_extended(void)
{
  if (mmix_rv_read() != MMIX_KERNEL_RV)
    panic("kinit extended");

  publish_zone(KALLOC_EXTENDED_ZONE, KALLOC_EXTENDED_START,
               KALLOC_EXTENDED_LIMIT, &kmem.extended_ready,
               "kinit extended", "kinit extended twice");
}

int
kalloc_page_is_managed(void *pa)
{
  int managed;
  int zone = page_zone((uint64)pa);

  acquire(&kmem.lock);
  managed = zone == KALLOC_LOW_ZONE ||
            (zone == KALLOC_RECLAIMED_ZONE && kmem.reclaimed_ready) ||
            (zone == KALLOC_EXTENDED_ZONE && kmem.extended_ready);
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
  if ((zone == KALLOC_RECLAIMED_ZONE && !kmem.reclaimed_ready) ||
      (zone == KALLOC_EXTENDED_ZONE && !kmem.extended_ready)) {
    release(&kmem.lock);
    panic("kfree unpublished");
  }
  release(&kmem.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.zone[zone].freelist;
  kmem.zone[zone].freelist = r;
  kmem.zone[zone].free_pages++;
  release(&kmem.lock);
}

static struct run *
alloc_from_zone(struct kalloc_zone *zone)
{
  struct run *r = zone->freelist;

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
  if (!dma_only && kmem.extended_ready)
    r = alloc_from_zone(&kmem.zone[KALLOC_EXTENDED_ZONE]);
  else
    r = 0;
  if (!dma_only && r == 0 && kmem.reclaimed_ready)
    r = alloc_from_zone(&kmem.zone[KALLOC_RECLAIMED_ZONE]);
  if (r == 0)
    r = alloc_from_zone(&kmem.zone[KALLOC_LOW_ZONE]);
  release(&kmem.lock);

  if (r != 0)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

// Allocate one 8192-byte MMIX physical page. Prefer Extended RAM, then the
// reclaimed bare-segment backing, and finally Low RAM.
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
  if (kmem.extended_ready)
    base = alloc_contiguous_from_zone(&kmem.zone[KALLOC_EXTENDED_ZONE],
                                      KALLOC_EXTENDED_START,
                                      KALLOC_EXTENDED_LIMIT, count);
  if (base == 0 && kmem.reclaimed_ready)
    base = alloc_contiguous_from_zone(&kmem.zone[KALLOC_RECLAIMED_ZONE],
                                      STACK_PHYS_BASE, STACK_PHYS_END,
                                      count);
  if (base == 0 && kmem.reclaimed_ready)
    base = alloc_contiguous_from_zone(&kmem.zone[KALLOC_RECLAIMED_ZONE],
                                      DATA_PHYS_BASE, DATA_PHYS_END,
                                      count);
  if (base == 0 && kmem.reclaimed_ready)
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
          kmem.zone[KALLOC_EXTENDED_ZONE].free_pages;
  release(&kmem.lock);
  return count;
}
