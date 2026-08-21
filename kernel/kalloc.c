// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 8192-byte MMIX pages.

#include "mmix.h"
#include "param.h"
#include "kalloc.h"
#include "spinlock.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char kernel_end[]; // First address after the loaded kernel.
                          // Defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  uint64 free_pages;
} kmem;

void
kinit()
{
  uint64 first = KALLOC_START((uint64)kernel_end);

  if ((first & (PGSIZE - 1)) != 0 || first < KERNEL_LOAD ||
      first >= KALLOC_LIMIT)
    panic("kinit");

  initlock(&kmem.lock, "kmem");
  freerange((void *)first, (void *)KALLOC_LIMIT);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

int
kalloc_page_is_managed(void *pa)
{
  uint64 address = (uint64)pa;
  uint64 first = KALLOC_START((uint64)kernel_end);

  return (address & (PGSIZE - 1)) == 0 && address >= first &&
         address < KALLOC_LIMIT;
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if (!kalloc_page_is_managed(pa))
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  kmem.free_pages++;
  release(&kmem.lock);
}

// Allocate one 8192-byte MMIX physical page.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    kmem.free_pages--;
  }
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

// Allocate count physically contiguous pages. This is needed for MMIX rV
// root blocks, which hardware addresses as one contiguous array.
void *
kalloc_contiguous(uint count)
{
  struct run *base;
  uint64 managed_pages =
    (KALLOC_LIMIT - KALLOC_START((uint64)kernel_end)) / PGSIZE;

  if (count == 0 || count > managed_pages)
    return 0;

  acquire(&kmem.lock);
  for (base = kmem.freelist; base != 0; base = base->next) {
    uint64 address = (uint64)base;
    uint found = 1;

    if (address > KALLOC_LIMIT - (uint64)count * PGSIZE)
      continue;
    for (uint page = 1; page < count; page++) {
      struct run *candidate;
      uint64 wanted = address + (uint64)page * PGSIZE;

      for (candidate = kmem.freelist; candidate != 0;
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
      struct run **link = &kmem.freelist;
      uint64 wanted = address + (uint64)page * PGSIZE;

      while ((uint64)*link != wanted)
        link = &(*link)->next;
      *link = (*link)->next;
    }
    kmem.free_pages -= count;
    release(&kmem.lock);

    memset((void *)address, 5, (uint64)count * PGSIZE);
    return (void *)address;
  }
  release(&kmem.lock);
  return 0;
}

uint64
kalloc_free_pages(void)
{
  uint64 count;

  acquire(&kmem.lock);
  count = kmem.free_pages;
  release(&kmem.lock);
  return count;
}
