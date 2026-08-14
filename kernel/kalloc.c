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

uint64
kalloc_free_pages(void)
{
  uint64 count;

  acquire(&kmem.lock);
  count = kmem.free_pages;
  release(&kmem.lock);
  return count;
}
