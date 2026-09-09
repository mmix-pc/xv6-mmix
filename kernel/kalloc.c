// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 8192-byte MMIX pages.

#include "mmix.h"
#include "kalloc.h"
#include "physmem.h"
#include "spinlock.h"
#include "defs.h"

struct run {
  struct run *next;
};

struct kalloc_span {
  uint64 start;
  uint64 limit;
};

static struct {
  struct spinlock lock;
  struct run *freelist;
  struct kalloc_span spans[PHYSMEM_MAX_SPANS];
  uint32 span_count;
  uint64 free_pages;
  uint64 managed_pages;
  uint64 physical_pages;
  uint64 initial_pages;
  int ready;
} kmem;

static int
span_index_locked(uint64 address)
{
  if ((address & (PGSIZE - 1)) != 0)
    return -1;
  for (uint32 index = 0; index < kmem.span_count; index++) {
    if (address >= kmem.spans[index].start && address < kmem.spans[index].limit)
      return index;
  }
  return -1;
}

static void
allocator_audit_locked(void)
{
  struct run *run = kmem.freelist;
  uint64 managed = 0;
  uint64 free = 0;

  if (!kmem.ready) {
    if (run != 0 || kmem.span_count != 0 || kmem.free_pages != 0 ||
        kmem.managed_pages != 0)
      panic("kalloc unpublished");
    return;
  }
  if (kmem.span_count == 0 || kmem.span_count > PHYSMEM_MAX_SPANS)
    panic("kalloc spans");
  for (uint32 index = 0; index < kmem.span_count; index++) {
    const struct kalloc_span *span = &kmem.spans[index];

    if (span->start >= span->limit ||
        ((span->start | span->limit) & (PGSIZE - 1)) != 0 ||
        (index != 0 && kmem.spans[index - 1].limit >= span->start))
      panic("kalloc spans");
    managed += (span->limit - span->start) / PGSIZE;
  }
  if (managed != kmem.managed_pages || kmem.free_pages > managed ||
      managed > physmem_managed_pages())
    panic("kalloc count");
  while (run != 0 && free <= kmem.free_pages) {
    if (span_index_locked((uint64)run) < 0)
      panic("kalloc freelist");
    run = run->next;
    free++;
  }
  if (free != kmem.free_pages)
    panic("kalloc freelist");
}

static void
publish_ranges(const struct physmem_span *ranges, uint32 range_count,
               uint64 page_count, int initial)
{
  struct kalloc_span combined[2 * PHYSMEM_MAX_SPANS];
  struct kalloc_span merged[PHYSMEM_MAX_SPANS];
  uint32 combined_count;
  uint32 merged_count = 0;
  uint64 counted_pages = 0;

  if (ranges == 0 || range_count == 0 || range_count > PHYSMEM_MAX_SPANS)
    panic("kalloc publish");

  acquire(&kmem.lock);
  if ((initial && kmem.ready) || (!initial && !kmem.ready) ||
      (!kmem.ready && (kmem.freelist != 0 || kmem.span_count != 0 ||
                       kmem.free_pages != 0 || kmem.managed_pages != 0))) {
    release(&kmem.lock);
    panic(initial ? "kinit twice" : "kalloc unpublished");
  }
  combined_count = kmem.span_count + range_count;
  if (combined_count > 2 * PHYSMEM_MAX_SPANS) {
    release(&kmem.lock);
    panic("kalloc capacity");
  }
  for (uint32 index = 0; index < kmem.span_count; index++)
    combined[index] = kmem.spans[index];
  for (uint32 index = 0; index < range_count; index++) {
    uint64 start = ranges[index].physical_base;
    uint64 size = ranges[index].size;

    // Concurrent releases can merge spans between separate index queries.
    if (size == 0 || size > ~start || ((start | size) & (PGSIZE - 1)) != 0 ||
        !physmem_contains(start, size)) {
      release(&kmem.lock);
      panic("kalloc range");
    }
    combined[kmem.span_count + index] = (struct kalloc_span){
      .start = start,
      .limit = start + size,
    };
    counted_pages += size / PGSIZE;
  }
  if (counted_pages != page_count) {
    release(&kmem.lock);
    panic("kalloc pages");
  }
  for (uint32 index = 1; index < combined_count; index++) {
    struct kalloc_span value = combined[index];
    uint32 position = index;

    while (position != 0 && combined[position - 1].start > value.start) {
      combined[position] = combined[position - 1];
      position--;
    }
    combined[position] = value;
  }
  for (uint32 index = 0; index < combined_count; index++) {
    const struct kalloc_span *current = &combined[index];

    if (merged_count != 0 && current->start < merged[merged_count - 1].limit) {
      release(&kmem.lock);
      panic("kalloc overlap");
    }
    if (merged_count != 0 && current->start == merged[merged_count - 1].limit) {
      merged[merged_count - 1].limit = current->limit;
      continue;
    }
    if (merged_count == PHYSMEM_MAX_SPANS) {
      release(&kmem.lock);
      panic("kalloc capacity");
    }
    merged[merged_count++] = *current;
  }

  // Newly published pages have no dangling references. Link them directly
  // instead of filling an entire RAM configuration during boot.
  for (uint32 index = 0; index < range_count; index++) {
    uint64 limit = ranges[index].physical_base + ranges[index].size;

    for (uint64 address = ranges[index].physical_base; address < limit;
         address += PGSIZE) {
      struct run *run = (struct run *)address;

      run->next = kmem.freelist;
      kmem.freelist = run;
      kmem.free_pages++;
    }
  }
  for (uint32 index = 0; index < merged_count; index++)
    kmem.spans[index] = merged[index];
  kmem.span_count = merged_count;
  kmem.managed_pages += page_count;
  if (initial) {
    kmem.initial_pages = page_count;
    kmem.physical_pages = page_count + physmem_reserved_pages();
  }
  kmem.ready = 1;
  allocator_audit_locked();
  release(&kmem.lock);
}

void
kinit(void)
{
  struct physmem_span spans[PHYSMEM_MAX_SPANS];
  uint32 count;
  uint64 pages = 0;
  int status;

  if (kmem.ready)
    panic("kinit twice");
  status = physmem_init();
  if (status != PHYSMEM_OK && status != PHYSMEM_ALREADY_INITIALIZED)
    panic("kinit plan");
  count = physmem_span_count();
  if (count == 0 || count > PHYSMEM_MAX_SPANS)
    panic("kinit spans");
  for (uint32 index = 0; index < count; index++) {
    if (physmem_span(index, &spans[index]) != PHYSMEM_OK)
      panic("kinit span");
    pages += spans[index].size / PGSIZE;
  }
  if (pages != physmem_managed_pages())
    panic("kinit pages");

  initlock(&kmem.lock, "kmem");
  publish_ranges(spans, count, pages, 1);
}

void
kalloc_publish_release(const struct physmem_release *released)
{
  if (released == 0)
    panic("kalloc release");
  if (released->span_count == 0) {
    if (released->page_count != 0)
      panic("kalloc release");
    acquire(&kmem.lock);
    if (!kmem.ready) {
      release(&kmem.lock);
      panic("kalloc unpublished");
    }
    release(&kmem.lock);
    return;
  }
  publish_ranges(released->spans, released->span_count, released->page_count,
                 0);
}

int
kalloc_page_is_managed(void *pa)
{
  int managed;

  acquire(&kmem.lock);
  managed = kmem.ready && span_index_locked((uint64)pa) >= 0;
  release(&kmem.lock);
  return managed;
}

int
kalloc_page_is_dma(void *pa)
{
  uint64 address = (uint64)pa;

  return address < (1ULL << MMIX_PHYS_BITS) && kalloc_page_is_managed(pa);
}

// Free the page of physical memory pointed at by pa, which normally should
// have been returned by a call to kalloc(). Newly published pages are linked
// internally and do not pass through this interface.
void
kfree(void *pa)
{
  struct run *run;

  acquire(&kmem.lock);
  if (!kmem.ready || span_index_locked((uint64)pa) < 0) {
    release(&kmem.lock);
    panic("kfree");
  }
  release(&kmem.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  run = (struct run *)pa;

  acquire(&kmem.lock);
  if (kmem.free_pages >= kmem.managed_pages) {
    release(&kmem.lock);
    panic("kfree count");
  }
  run->next = kmem.freelist;
  kmem.freelist = run;
  kmem.free_pages++;
  release(&kmem.lock);
}

static struct run *
alloc_page_locked(int dma_only)
{
  struct run **link = &kmem.freelist;

  while (*link != 0 && dma_only && (uint64)*link >= (1ULL << MMIX_PHYS_BITS))
    link = &(*link)->next;
  if (*link == 0)
    return 0;
  struct run *run = *link;
  *link = run->next;
  kmem.free_pages--;
  return run;
}

static void *
kalloc_from(int dma_only)
{
  struct run *run;

  acquire(&kmem.lock);
  if (!kmem.ready) {
    release(&kmem.lock);
    panic("kalloc unpublished");
  }
  run = alloc_page_locked(dma_only);
  release(&kmem.lock);

  if (run != 0)
    memset((char *)run, 5, PGSIZE); // fill with junk
  return (void *)run;
}

// Allocate one 8192-byte MMIX physical page.
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

// Sort by descending physical address without allocating memory or recursing.
// Ordinary page allocation/free keeps its existing constant-time list updates.
static void
sort_freelist_locked(void)
{
  for (uint64 width = 1; width < kmem.free_pages; width *= 2) {
    struct run *left = kmem.freelist;
    struct run *head = 0;
    struct run **tail = &head;

    while (left != 0) {
      struct run *right = left;
      uint64 nleft = 0, nright = width;

      while (nleft < width && right != 0) {
        right = right->next;
        nleft++;
      }
      while (nleft != 0 || (nright != 0 && right != 0)) {
        struct run *next;

        if (nleft != 0 && (nright == 0 || right == 0 ||
                          (uint64)left > (uint64)right)) {
          next = left;
          left = left->next;
          nleft--;
        } else {
          next = right;
          right = right->next;
          nright--;
        }
        *tail = next;
        tail = &next->next;
      }
      left = right;
    }
    *tail = 0;
    kmem.freelist = head;
  }
}

// A single pass finds runs already linked in descending address order, as
// freshly published pages are. After sorting, this finds every possible run.
static void *
take_contiguous_locked(uint count)
{
  struct run **link = &kmem.freelist;
  struct run *run = *link;
  uint length = 0;

  while (run != 0) {
    length++;
    if (length == count) {
      *link = run->next;
      kmem.free_pages -= count;
      return run;
    }
    if ((uint64)run != (uint64)run->next + PGSIZE) {
      link = &run->next;
      length = 0;
    }
    run = run->next;
  }
  return 0;
}

static void *
alloc_contiguous_locked(uint count)
{
  void *base;

  if (count > kmem.free_pages)
    return 0;
  base = take_contiguous_locked(count);
  if (base != 0)
    return base;

  // Avoid repeated whole-list membership searches with interrupts disabled:
  // fragmented lists now cost O(n log n), rather than O(n squared).
  sort_freelist_locked();
  return take_contiguous_locked(count);
}

// Allocate count physically contiguous pages. A run never crosses an
// unpublished physical-memory interval.
void *
kalloc_contiguous(uint count)
{
  void *base;

  if (count == 0)
    return 0;
  acquire(&kmem.lock);
  if (!kmem.ready) {
    release(&kmem.lock);
    panic("kalloc unpublished");
  }
  base = alloc_contiguous_locked(count);
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
  if (!kmem.ready) {
    release(&kmem.lock);
    panic("kalloc unpublished");
  }
  count = kmem.free_pages;
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
  stats->physical_pages = kmem.physical_pages;
  stats->managed_pages = kmem.managed_pages;
  stats->free_pages = kmem.free_pages;
  stats->reserved_pages = kmem.physical_pages - kmem.managed_pages;
  stats->used_pages = kmem.managed_pages - kmem.free_pages;
  stats->reclaimed_pages = kmem.managed_pages - kmem.initial_pages;
  release(&kmem.lock);
}
