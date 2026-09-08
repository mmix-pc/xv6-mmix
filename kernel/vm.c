#include "mmix.h"
#include "boot.h"
#include "platform.h"
#include "defs.h"
#include "diagnostic.h"
#include "kalloc.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

#define MMIX_PT_LEVEL1_SPAN          (PGSIZE * MMIX_PT_ENTRIES)
#define MMIX_KERNEL_LOW_CHILDREN     (LOW_RAM_END / MMIX_PT_LEVEL1_SPAN - 1)
#define MMIX_KERNEL_BARE_CHILDREN                                         \
  (BARE_SEGMENT_BACKING_SIZE / MMIX_PT_LEVEL1_SPAN)

struct kernel_mapping_range {
  uint64 base;
  uint64 size;
};

static const struct kernel_mapping_range kernel_devices[] = {
  {UART0_BASE, UART0_SIZE},
  {VIRTIO0_BASE, VIRTIO0_SIZE},
  {FRAMEBUFFER_CONTROL_BASE, FRAMEBUFFER_CONTROL_SIZE},
  {TIMER_BASE, TIMER_SIZE},
  {INTC_BASE, INTC_SIZE},
  {IPI_BASE, IPI_SIZE},
};

extern char kernel_text_end[];
extern char kernel_rodata_end[];
extern char kernel_end[];

static struct mmix_pagetable kernel_table;
pagetable_t kernel_pagetable = &kernel_table;

_Static_assert((LOW_RAM_END % MMIX_PT_LEVEL1_SPAN) == 0,
               "Low RAM must end on a level-1 table span");
_Static_assert((BARE_SEGMENT_BACKING_BASE % MMIX_PT_LEVEL1_SPAN) == 0 &&
                 (BARE_SEGMENT_BACKING_LIMIT % MMIX_PT_LEVEL1_SPAN) == 0 &&
                 MMIX_KERNEL_BARE_CHILDREN == 17,
               "bare-segment backing must cover 17 level-1 spans");
_Static_assert((MMIO_BASE % MMIX_PT_LEVEL1_SPAN) == 0 &&
                 KERNEL_IDENTITY_LIMIT ==
                   (uint64)MMIX_PT_LEVEL1_SPAN * MMIX_PT_ENTRIES,
               "upper kernel map must fit the level-1 root");
_Static_assert(UART0_BASE / MMIX_PT_LEVEL1_SPAN ==
                 (MMIO_DEVICE_PAGE_END - 1) / MMIX_PT_LEVEL1_SPAN,
               "kernel devices must share one level-1 child table");
_Static_assert(MMIO_DEVICE_PAGE_END <= MMIO_APERTURE_END,
               "device pages must remain inside the MMIO aperture");

enum walk_status {
  WALK_OK,
  WALK_ABSENT,
  WALK_INVALID,
  WALK_NOMEM,
};

static pte_t *
table_address(uint64 pa)
{
  return (pte_t *)mmix_phys_alias(pa);
}

static int
managed_page_range(uint64 start, uint pages)
{
  for (uint page = 0; page < pages; page++)
    if (!kalloc_page_is_managed((void *)(start + (uint64)page * PGSIZE)))
      return 0;
  return 1;
}

static int
pagetable_valid(pagetable_t pagetable)
{
  uint64 root_pa;
  uint64 asn;

  if (pagetable == 0)
    return 0;
  if (pagetable == kernel_pagetable)
    return pagetable->rv == MMIX_KERNEL_RV;

  root_pa = MMIX_RV_ROOT_PA(pagetable->rv);
  asn = MMIX_RV_N(pagetable->rv);
  return kalloc_page_is_managed(pagetable) &&
         managed_page_range(root_pa, MMIX_USER_ROOT_BLOCKS) &&
         asn >= MMIX_USER_ASN_FIRST && asn <= MMIX_USER_ASN_LAST &&
         MMIX_RV_F(pagetable->rv) <= MMIX_RV_F_SOFTWARE &&
         (pagetable->rv & ~MMIX_RV_F_VALUE_MASK) ==
           mmix_user_rv_make(root_pa, asn);
}

static int
user_pagetable(pagetable_t pagetable)
{
  return pagetable_valid(pagetable) && pagetable != kernel_pagetable;
}

static void
segment_bounds(uint64 rv, uint segment, uint *start, uint *end)
{
  uint boundary[5] = {0, MMIX_RV_B1(rv), MMIX_RV_B2(rv), MMIX_RV_B3(rv),
                      MMIX_RV_B4(rv)};

  *start = boundary[segment];
  *end = boundary[segment + 1];
  if (*end < *start)
    *end = *start;
}

static int
user_page_mappable(uint64 va)
{
  return (va >= MMIX_USER_IMAGE_BASE && va < MMIX_USER_HEAP_LIMIT) ||
         (va >= MMIX_USER_STACK_BASE && va < MMIX_USER_STACK_TOP) ||
         (va >= MMIX_USER_REGISTER_STACK_BASE &&
          va < MMIX_USER_REGISTER_STACK_TOP);
}

static int
user_range_mappable(uint64 first, uint64 last)
{
  return (first >= MMIX_USER_IMAGE_BASE && last < MMIX_USER_HEAP_LIMIT) ||
         (first >= MMIX_USER_STACK_BASE && last < MMIX_USER_STACK_TOP) ||
         (first >= MMIX_USER_REGISTER_STACK_BASE &&
          last < MMIX_USER_REGISTER_STACK_TOP);
}

static int
permissions_valid(uint64 permissions)
{
  if (permissions == 0 || (permissions & ~MMIX_PTE_PERM_VALUE_MASK) != 0)
    return 0;
  if ((permissions & PTE_W) != 0 && (permissions & PTE_R) == 0)
    return 0;
  return (permissions & (PTE_W | PTE_X)) != (PTE_W | PTE_X);
}

static int
ptp_valid(pagetable_t pagetable, pte_t ptp)
{
  uint64 child_pa = mmix_ptp_child_pa(ptp);
  uint64 asn = MMIX_RV_N(pagetable->rv);

  return ptp == mmix_ptp_make(child_pa, asn) &&
         kalloc_page_is_managed((void *)child_pa);
}

static int
leaf_valid(pagetable_t pagetable, pte_t pte)
{
  uint64 pa = mmix_pte_pa(pte);
  uint64 permissions = mmix_pte_permissions(pte);
  uint64 asn = MMIX_RV_N(pagetable->rv);

  return pte == mmix_pte_make(pa, asn, permissions) &&
         permissions_valid(permissions) &&
         (!user_pagetable(pagetable) ||
          kalloc_page_is_managed((void *)pa));
}

static int
walk_leaf(pagetable_t pagetable, uint64 va, int alloc, pte_t **leaf)
{
  uint64 page;
  uint64 digits[MMIX_KERNEL_B1];
  uint64 highest;
  uint64 table_pa;
  uint64 asn;
  uint segment;
  uint start;
  uint end;
  uint levels;

  if (!pagetable_valid(pagetable))
    return WALK_INVALID;

  segment = mmix_va_segment(va);
  if (segment >= 4)
    return WALK_INVALID;
  segment_bounds(pagetable->rv, segment, &start, &end);
  levels = end - start;
  if (levels == 0 || levels > MMIX_KERNEL_B1 ||
      mmix_va_page(va) >= (1L << (levels * MMIX_PT_INDEX_BITS)))
    return WALK_INVALID;
  if (user_pagetable(pagetable) && !user_page_mappable(PGROUNDDOWN(va)))
    return WALK_INVALID;

  page = mmix_va_page(va);
  for (uint level = 0; level < levels; level++)
    digits[level] = (page >> (level * MMIX_PT_INDEX_BITS)) & MMIX_PT_INDEX_MASK;

  // The highest nonzero digit selects its contiguous root block. Lower
  // nonzero levels are reached through allocator-owned PTP blocks.
  highest = 0;
  for (uint level = 1; level < levels; level++)
    if (digits[level] != 0)
      highest = level;
  table_pa = MMIX_RV_ROOT_PA(pagetable->rv) + (start + highest) * PGSIZE;
  asn = MMIX_RV_N(pagetable->rv);

  for (uint level = highest; level > 0; level--) {
    pte_t *entry = &table_address(table_pa)[digits[level]];
    void *child;

    if (*entry == 0) {
      if (!alloc)
        return WALK_ABSENT;
      child = kalloc();
      if (child == 0)
        return WALK_NOMEM;
      memset(table_address((uint64)child), 0, PGSIZE);
      *entry = mmix_ptp_make((uint64)child, asn);
    } else if (!ptp_valid(pagetable, *entry)) {
      return WALK_INVALID;
    }
    table_pa = mmix_ptp_child_pa(*entry);
  }

  *leaf = &table_address(table_pa)[digits[0]];
  return WALK_OK;
}

static void
mmix_pagetable_sync(pagetable_t pagetable)
{
  if (pagetable_valid(pagetable) && mmix_rv_read() == pagetable->rv)
    mmix_rv_publish(pagetable->rv);
}

pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  pte_t *leaf;

  if (walk_leaf(pagetable, va, alloc, &leaf) != WALK_OK)
    return 0;
  return leaf;
}

static int
mmix_pagetable_translate(pagetable_t pagetable, uint64 va, uint64 permissions,
                         uint64 *pa)
{
  pte_t *leaf;

  if (pa == 0 || permissions == 0 ||
      (permissions & ~MMIX_PTE_PERM_VALUE_MASK) != 0 ||
      walk_leaf(pagetable, va, 0, &leaf) != WALK_OK ||
      !leaf_valid(pagetable, *leaf) ||
      (mmix_pte_permissions(*leaf) & permissions) != permissions)
    return -1;

  *pa = mmix_pte_pa(*leaf) | (va & (PGSIZE - 1));
  return 0;
}

uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  uint64 pa;

  if (mmix_pagetable_translate(pagetable, va, PTE_R, &pa) < 0)
    return 0;
  return PGROUNDDOWN(pa);
}

int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 last_va;
  uint64 last_pa;
  uint64 current_va;
  uint64 current_pa;
  pte_t *leaf;

  if ((va & (PGSIZE - 1)) != 0 || (pa & (PGSIZE - 1)) != 0 ||
      (size & (PGSIZE - 1)) != 0 || size == 0 ||
      !permissions_valid((uint64)perm) || va + size < va || pa + size < pa)
    return -1;

  last_va = va + size - PGSIZE;
  last_pa = pa + size - PGSIZE;
  if (!pagetable_valid(pagetable) ||
      mmix_va_segment(va) != mmix_va_segment(last_va) ||
      last_pa > MMIX_PTE_PA_FIELD_MASK ||
      (user_pagetable(pagetable) &&
       !user_range_mappable(va, last_va)))
    return -1;

  current_va = va;
  current_pa = pa;
  for (;;) {
    if (walk_leaf(pagetable, current_va, 1, &leaf) != WALK_OK || *leaf != 0) {
      mmix_pagetable_sync(pagetable);
      return -1;
    }
    *leaf = mmix_pte_make(current_pa, MMIX_RV_N(pagetable->rv), perm);
    if (current_va == last_va)
      break;
    current_va += PGSIZE;
    current_pa += PGSIZE;
  }

  mmix_pagetable_sync(pagetable);
  return 0;
}

static int
mmix_unmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free,
           struct vm_reclaim *reclaim)
{
  uint64 size;
  int changed = 0;

  if ((va & (PGSIZE - 1)) != 0 || npages > ~(uint64)0 / PGSIZE)
    return -1;
  size = npages * PGSIZE;
  if (va + size < va)
    return -1;
  if (!pagetable_valid(pagetable) ||
      (npages != 0 &&
       (mmix_va_segment(va) != mmix_va_segment(va + size - PGSIZE) ||
        (user_pagetable(pagetable) &&
         !user_range_mappable(va, va + size - PGSIZE)))))
    return -1;

  for (uint64 current = va; current < va + size; current += PGSIZE) {
    pte_t *leaf;
    int status = walk_leaf(pagetable, current, 0, &leaf);

    if (status == WALK_ABSENT)
      continue;
    if (status != WALK_OK) {
      if (changed)
        mmix_pagetable_sync(pagetable);
      return -1;
    }
    if (*leaf == 0)
      continue;
    if (!leaf_valid(pagetable, *leaf)) {
      if (changed)
        mmix_pagetable_sync(pagetable);
      return -1;
    }
    if (do_free) {
      void *page = (void *)mmix_pte_pa(*leaf);

      if (!kalloc_page_is_managed(page)) {
        if (changed)
          mmix_pagetable_sync(pagetable);
        return -1;
      }
      *leaf = 0;
      if (reclaim != 0) {
        if (reclaim->count == ~0ULL)
          panic("unmap reclaim count");
        *(void **)page = reclaim->pages;
        reclaim->pages = page;
        reclaim->count++;
      } else {
        kfree(page);
      }
    } else {
      *leaf = 0;
    }
    changed = 1;
  }

  if (changed)
    mmix_pagetable_sync(pagetable);
  return 0;
}

void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  if (mmix_unmap(pagetable, va, npages, do_free, 0) < 0)
    panic("uvmunmap");
}

static int
free_table(pagetable_t pagetable, uint64 table_pa, uint level)
{
  pte_t *table = table_address(table_pa);

  for (uint index = 0; index < MMIX_PT_ENTRIES; index++) {
    pte_t entry = table[index];
    uint64 child_pa;

    if (entry == 0)
      continue;
    if (level == 0 || !ptp_valid(pagetable, entry))
      return -1;
    child_pa = mmix_ptp_child_pa(entry);
    if (free_table(pagetable, child_pa, level - 1) < 0)
      return -1;
    table[index] = 0;
    kfree((void *)child_pa);
  }
  return 0;
}

static int
retire_table(pagetable_t pagetable, uint64 table_pa, uint level, uint64 base,
             uint64 asn, uint64 *result)
{
  pte_t *table = table_address(table_pa);
  uint64 span = PGSIZE;

  for (uint current = 0; current < level; current++)
    span *= MMIX_PT_ENTRIES;
  for (uint index = 0; index < MMIX_PT_ENTRIES; index++) {
    pte_t entry = table[index];

    if (entry == 0)
      continue;
    if (level == 0) {
      if (!leaf_valid(pagetable, entry))
        return -1;
      *result |= mmix_ldvts((base + (uint64)index * PGSIZE) | (asn << 3));
      continue;
    }
    if (!ptp_valid(pagetable, entry) ||
        retire_table(pagetable, mmix_ptp_child_pa(entry), level - 1,
                     base + (uint64)index * span, asn, result) < 0)
      return -1;
  }
  return 0;
}

// Invalidate every mapped leaf of one user address space on this CPU. The
// caller keeps the page-table tree stable until all target CPUs complete.
int
uvmretire_local(pagetable_t pagetable, uint64 *result)
{
  uint64 root_pa;
  uint64 asn;

  if (!user_pagetable(pagetable) || result == 0)
    return -1;
  root_pa = MMIX_RV_ROOT_PA(pagetable->rv);
  asn = MMIX_RV_N(pagetable->rv);
  *result = 0;
  for (uint root = 0; root < MMIX_USER_ROOT_BLOCKS; root++) {
    int found = 0;

    for (uint segment = 0; segment < 4; segment++) {
      uint start;
      uint end;

      segment_bounds(pagetable->rv, segment, &start, &end);
      if (root < start || root >= end)
        continue;
      if (retire_table(pagetable, root_pa + (uint64)root * PGSIZE,
                       root - start, (uint64)segment << 61, asn, result) < 0)
        return -1;
      found = 1;
      break;
    }
    if (!found)
      return -1;
  }
  return (*result & ~VM_INVALIDATE_ALL) == 0 ? 0 : -1;
}

static int
mmix_pagetable_destroy(pagetable_t pagetable)
{
  uint64 root_pa;
  uint root_blocks;

  if (!pagetable_valid(pagetable))
    return -1;
  root_pa = MMIX_RV_ROOT_PA(pagetable->rv);
  root_blocks = user_pagetable(pagetable) ? MMIX_USER_ROOT_BLOCKS
                                         : MMIX_KERNEL_ROOT_BLOCKS;
  for (uint root = 0; root < root_blocks; root++) {
    uint level = 0;
    int found = 0;

    for (uint segment = 0; segment < 4; segment++) {
      uint start;
      uint end;

      segment_bounds(pagetable->rv, segment, &start, &end);
      if (root >= start && root < end) {
        level = root - start;
        found = 1;
        break;
      }
    }
    if (!found ||
        free_table(pagetable, root_pa + root * PGSIZE, level) < 0) {
      mmix_pagetable_sync(pagetable);
      return -1;
    }
  }

  mmix_pagetable_sync(pagetable);
  return 0;
}

void
freewalk(pagetable_t pagetable)
{
  if (mmix_pagetable_destroy(pagetable) < 0)
    panic("freewalk");
}

pagetable_t
uvmcreate(uint asn)
{
  pagetable_t pagetable;
  void *roots;

  if (asn < MMIX_USER_ASN_FIRST || asn > MMIX_USER_ASN_LAST)
    return 0;
  pagetable = kalloc();
  if (pagetable == 0)
    return 0;
  roots = kalloc_contiguous(MMIX_USER_ROOT_BLOCKS);
  if (roots == 0) {
    kfree(pagetable);
    return 0;
  }

  memset(roots, 0, MMIX_USER_ROOT_BLOCKS * PGSIZE);
  memset(pagetable, 0, PGSIZE);
  pagetable->rv = mmix_user_rv_make((uint64)roots, asn);
  if (!pagetable_valid(pagetable)) {
    for (uint page = 0; page < MMIX_USER_ROOT_BLOCKS; page++)
      kfree((char *)roots + page * PGSIZE);
    kfree(pagetable);
    return 0;
  }
  return pagetable;
}

uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int permissions)
{
  uint64 first;
  uint64 current;
  uint64 mapped = 0;
  uint64 leaf_permissions = (uint64)permissions | PTE_R;

  if (!user_pagetable(pagetable) || newsz < oldsz ||
      newsz > MMIX_USER_HEAP_LIMIT || !permissions_valid(leaf_permissions))
    return 0;

  first = PGROUNDUP(oldsz);
  if (first < MMIX_USER_IMAGE_BASE)
    first = MMIX_USER_IMAGE_BASE;
  for (current = first; current < newsz; current += PGSIZE) {
    void *page = kalloc();

    if (page == 0)
      goto fail;
    memset(page, 0, PGSIZE);
    if (mappages(pagetable, current, PGSIZE, (uint64)page,
                 leaf_permissions) < 0) {
      kfree(page);
      goto fail;
    }
    mapped++;
  }
  return newsz;

fail:
  if (mapped != 0 && mmix_unmap(pagetable, first, mapped, 1, 0) < 0)
    panic("uvmalloc rollback");
  return 0;
}

static uint64
uvmdealloc_internal(pagetable_t pagetable, uint64 oldsz, uint64 newsz,
                    struct vm_reclaim *reclaim)
{
  uint64 first;
  uint64 last;

  if (!user_pagetable(pagetable) || oldsz > MMIX_USER_HEAP_LIMIT)
    return oldsz;
  if (newsz >= oldsz)
    return oldsz;

  first = PGROUNDUP(newsz);
  if (first < MMIX_USER_IMAGE_BASE)
    first = MMIX_USER_IMAGE_BASE;
  last = PGROUNDUP(oldsz);
  if (last > first && mmix_unmap(pagetable, first,
                                 (last - first) / PGSIZE, 1, reclaim) < 0)
    panic("uvmdealloc");
  return newsz;
}

uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  return uvmdealloc_internal(pagetable, oldsz, newsz, 0);
}

uint64
uvmdealloc_deferred(pagetable_t pagetable, uint64 oldsz, uint64 newsz,
                    struct vm_reclaim *reclaim)
{
  if (reclaim == 0 || reclaim->pages != 0 || reclaim->count != 0)
    panic("uvmdealloc deferred");
  return uvmdealloc_internal(pagetable, oldsz, newsz, reclaim);
}

void
uvmreclaim(struct vm_reclaim *reclaim)
{
  uint64 count = 0;

  if (reclaim == 0)
    panic("uvmreclaim");
  while (reclaim->pages != 0) {
    void *page = reclaim->pages;

    if (count >= reclaim->count || !kalloc_page_is_managed(page))
      panic("uvmreclaim page");
    reclaim->pages = *(void **)page;
    kfree(page);
    count++;
  }
  if (count != reclaim->count)
    panic("uvmreclaim count");
  reclaim->count = 0;
}

// Resolve a software-translation miss. Existing mappings are returned when
// they permit the access; only an absent page in the current process's logical
// heap may be materialized as demand-zero memory.
uint64
vmfault(pagetable_t pagetable, uint64 va, int permissions)
{
  struct proc *p = myproc();
  uint64 page_va = PGROUNDDOWN(va);
  uint64 result;
  pte_t *leaf;
  int status;
  void *page;

  if (p == 0 || pagetable == 0 || pagetable != p->pagetable || !intr_get())
    return 0;
  proc_vm_begin_mutation(p);
  if (p->state != RUNNING || p->vm_owner_cpu != cpuid() || pagetable == 0 ||
      pagetable != p->pagetable ||
      MMIX_RV_F(pagetable->rv) != MMIX_RV_F_SOFTWARE ||
      (permissions != 0 && !permissions_valid((uint64)permissions)))
    goto fail;

  status = walk_leaf(pagetable, page_va, 0, &leaf);
  if (status == WALK_OK && *leaf != 0) {
    if (!leaf_valid(pagetable, *leaf) ||
        (permissions != 0 &&
         (mmix_pte_permissions(*leaf) & (uint64)permissions) !=
           (uint64)permissions))
      goto fail;
    result = *leaf;
    proc_vm_cancel_mutation(p);
    return result;
  }
  if ((status != WALK_ABSENT && status != WALK_OK) ||
      (status == WALK_OK && *leaf != 0) || permissions == 0 ||
      (permissions & PTE_X) != 0 ||
      p->lazy_start == 0 || va < p->lazy_start || va >= p->sz)
    goto fail;

  page = kalloc();
  if (page == 0)
    goto fail;
  memset(page, 0, PGSIZE);
  if (mappages(pagetable, page_va, PGSIZE, (uint64)page,
               PTE_R | PTE_W) < 0) {
    kfree(page);
    goto fail;
  }
  leaf = walk(pagetable, page_va, 0);
  if (leaf == 0 || !leaf_valid(pagetable, *leaf))
    panic("vmfault mapping");
  result = *leaf;
  proc_vm_commit_mutation(p, page_va, page_va + PGSIZE,
                          VM_INVALIDATE_DATA);
  return result;

fail:
  proc_vm_cancel_mutation(p);
  return 0;
}

static int
uvmalloc_range(pagetable_t pagetable, uint64 start, uint64 end)
{
  uint64 current;

  for (current = start; current < end; current += PGSIZE) {
    void *page = kalloc();

    if (page == 0)
      break;
    memset(page, 0, PGSIZE);
    if (mappages(pagetable, current, PGSIZE, (uint64)page,
                 PTE_R | PTE_W) < 0) {
      kfree(page);
      break;
    }
  }
  if (current == end)
    return 0;
  if (current > start &&
      mmix_unmap(pagetable, start, (current - start) / PGSIZE, 1, 0) < 0)
    panic("uvmalloc_range");
  return -1;
}

int
uvmallocstacks(pagetable_t pagetable)
{
  if (!user_pagetable(pagetable) ||
      uvmalloc_range(pagetable, MMIX_USER_STACK_BASE,
                     MMIX_USER_STACK_TOP) < 0)
    return -1;
  if (uvmalloc_range(pagetable, MMIX_USER_REGISTER_STACK_BASE,
                     MMIX_USER_REGISTER_STACK_TOP) < 0) {
    if (mmix_unmap(pagetable, MMIX_USER_STACK_BASE, MMIX_USER_STACK_PAGES,
                   1, 0) < 0)
      panic("uvmallocstacks");
    return -1;
  }
  return 0;
}

void
uvmclear(pagetable_t pagetable, uint64 va)
{
  uint64 page = PGROUNDDOWN(va);
  pte_t *leaf;

  if (!user_pagetable(pagetable) ||
      walk_leaf(pagetable, page, 0, &leaf) != WALK_OK ||
      !leaf_valid(pagetable, *leaf) ||
      mmix_unmap(pagetable, page, 1, 1, 0) < 0)
    panic("uvmclear");
}

static int
uvmcopy_range(pagetable_t old, pagetable_t new, uint64 start, uint64 end)
{
  for (uint64 va = start; va < end; va += PGSIZE) {
    pte_t *leaf;
    void *page;
    int status = walk_leaf(old, va, 0, &leaf);

    if (status == WALK_ABSENT || (status == WALK_OK && *leaf == 0))
      continue;
    if (status != WALK_OK || !leaf_valid(old, *leaf))
      return -1;
    page = kalloc();
    if (page == 0)
      return -1;
    memmove((void *)mmix_phys_alias((uint64)page),
            (void *)mmix_phys_alias(mmix_pte_pa(*leaf)), PGSIZE);
    if (mappages(new, va, PGSIZE, (uint64)page,
                 mmix_pte_permissions(*leaf)) < 0) {
      kfree(page);
      return -1;
    }
  }
  return 0;
}

static void
uvmremove_range(pagetable_t pagetable, uint64 start, uint64 end)
{
  if (end > start &&
      mmix_unmap(pagetable, start, (end - start) / PGSIZE, 1, 0) < 0)
    panic("uvmremove_range");
}

static int
uvmrange_empty(pagetable_t pagetable, uint64 start, uint64 end)
{
  for (uint64 va = start; va < end; va += PGSIZE) {
    pte_t *leaf;
    int status = walk_leaf(pagetable, va, 0, &leaf);

    if (status != WALK_ABSENT && (status != WALK_OK || *leaf != 0))
      return 0;
  }
  return 1;
}

int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  uint64 image_end;

  if (!user_pagetable(old) || !user_pagetable(new) || old == new ||
      sz > MMIX_USER_HEAP_LIMIT)
    return -1;
  image_end = PGROUNDUP(sz);
  if (image_end < MMIX_USER_IMAGE_BASE)
    image_end = MMIX_USER_IMAGE_BASE;
  if (!uvmrange_empty(new, MMIX_USER_IMAGE_BASE, image_end) ||
      !uvmrange_empty(new, MMIX_USER_STACK_BASE, MMIX_USER_STACK_TOP) ||
      !uvmrange_empty(new, MMIX_USER_REGISTER_STACK_BASE,
                      MMIX_USER_REGISTER_STACK_TOP))
    return -1;

  if (uvmcopy_range(old, new, MMIX_USER_IMAGE_BASE, image_end) == 0 &&
      uvmcopy_range(old, new, MMIX_USER_STACK_BASE,
                    MMIX_USER_STACK_TOP) == 0 &&
      uvmcopy_range(old, new, MMIX_USER_REGISTER_STACK_BASE,
                    MMIX_USER_REGISTER_STACK_TOP) == 0) {
    new->rv = mmix_user_rv_set_function(new->rv, MMIX_RV_F(old->rv));
    return 0;
  }

  uvmremove_range(new, MMIX_USER_IMAGE_BASE, image_end);
  uvmremove_range(new, MMIX_USER_STACK_BASE, MMIX_USER_STACK_TOP);
  uvmremove_range(new, MMIX_USER_REGISTER_STACK_BASE,
                  MMIX_USER_REGISTER_STACK_TOP);
  return -1;
}

void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  uint64 root_pa;
  uint64 image_end;

  if (!user_pagetable(pagetable) || sz > MMIX_USER_HEAP_LIMIT ||
      mmix_rv_read() == pagetable->rv)
    panic("uvmfree");
  root_pa = MMIX_RV_ROOT_PA(pagetable->rv);
  image_end = PGROUNDUP(sz);
  if (image_end < MMIX_USER_IMAGE_BASE)
    image_end = MMIX_USER_IMAGE_BASE;

  uvmremove_range(pagetable, MMIX_USER_IMAGE_BASE, image_end);
  uvmremove_range(pagetable, MMIX_USER_STACK_BASE, MMIX_USER_STACK_TOP);
  uvmremove_range(pagetable, MMIX_USER_REGISTER_STACK_BASE,
                  MMIX_USER_REGISTER_STACK_TOP);
  if (mmix_pagetable_destroy(pagetable) < 0)
    panic("uvmfree tables");
  for (uint page = 0; page < MMIX_USER_ROOT_BLOCKS; page++)
    kfree((void *)(root_pa + page * PGSIZE));
  kfree(pagetable);
}

int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  if (len != 0 && dstva > ~(uint64)0 - (len - 1))
    return -1;
  while (len != 0) {
    uint64 pa;
    uint64 count;

    if (mmix_pagetable_translate(pagetable, dstva, PTE_W, &pa) < 0) {
      if (vmfault(pagetable, dstva, PTE_R | PTE_W) == 0 ||
          mmix_pagetable_translate(pagetable, dstva, PTE_W, &pa) < 0)
        return -1;
    }
    count = PGSIZE - (dstva & (PGSIZE - 1));
    if (count > len)
      count = len;
    memmove((void *)mmix_phys_alias(pa), src, count);
    if (dstva + count < dstva)
      return -1;
    dstva += count;
    src += count;
    len -= count;
  }
  return 0;
}

int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  if (len != 0 && srcva > ~(uint64)0 - (len - 1))
    return -1;
  while (len != 0) {
    uint64 pa;
    uint64 count;

    if (mmix_pagetable_translate(pagetable, srcva, PTE_R, &pa) < 0) {
      if (vmfault(pagetable, srcva, PTE_R) == 0 ||
          mmix_pagetable_translate(pagetable, srcva, PTE_R, &pa) < 0)
        return -1;
    }
    count = PGSIZE - (srcva & (PGSIZE - 1));
    if (count > len)
      count = len;
    memmove(dst, (void *)mmix_phys_alias(pa), count);
    if (srcva + count < srcva)
      return -1;
    srcva += count;
    dst += count;
    len -= count;
  }
  return 0;
}

int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  if (max != 0 && srcva > ~(uint64)0 - (max - 1))
    return -1;
  while (max != 0) {
    uint64 pa;
    uint64 count;

    if (mmix_pagetable_translate(pagetable, srcva, PTE_R, &pa) < 0) {
      if (vmfault(pagetable, srcva, PTE_R) == 0 ||
          mmix_pagetable_translate(pagetable, srcva, PTE_R, &pa) < 0)
        return -1;
    }
    count = PGSIZE - (srcva & (PGSIZE - 1));
    if (count > max)
      count = max;
    while (count != 0) {
      char c = *(char *)mmix_phys_alias(pa++);

      *dst++ = c;
      max--;
      count--;
      srcva++;
      if (c == 0)
        return 0;
    }
  }
  return -1;
}

static int
ranges_overlap(uint64 first_base, uint64 first_size, uint64 second_base,
               uint64 second_size)
{
  return first_size != 0 && second_size != 0 &&
         first_base < second_base + second_size &&
         second_base < first_base + first_size;
}

static int
kernel_device_page(uint64 va)
{
  for (uint index = 0;
       index < sizeof(kernel_devices) / sizeof(kernel_devices[0]); index++)
    if (ranges_overlap(va, PGSIZE, kernel_devices[index].base,
                       kernel_devices[index].size))
      return 1;
  return 0;
}

static int
kernel_device_span(uint64 base, uint64 size)
{
  for (uint index = 0;
       index < sizeof(kernel_devices) / sizeof(kernel_devices[0]); index++) {
    uint64 device_page = PGROUNDDOWN(kernel_devices[index].base);
    uint64 device_limit =
      PGROUNDUP(kernel_devices[index].base + kernel_devices[index].size);

    if (ranges_overlap(base, size, device_page, device_limit - device_page))
      return 1;
  }
  return 0;
}

static uint64
kernel_high_ram_size(void)
{
  struct platform_physical_range ram;

  if (platform_ram(&ram) != PLATFORM_OK)
    return 0;
  return ram.size > PHYSICAL_LOW_RAM_SIZE
           ? ram.size - PHYSICAL_LOW_RAM_SIZE
           : 0;
}

static int
kernel_physical_memory_valid(void)
{
  struct platform_physical_range ram;
  uint64 high_size;

  if (platform_ram(&ram) != PLATFORM_OK ||
      ram.physical_base != PHYSICAL_LOW_RAM_BASE ||
      ram.size < PHYSICAL_LOW_RAM_SIZE ||
      (ram.size & (PGSIZE - 1)) != 0)
    return 0;
  high_size = ram.size - PHYSICAL_LOW_RAM_SIZE;
  return high_size <= KERNEL_IDENTITY_LIMIT - PHYSICAL_HIGH_RAM_BASE;
}

static pte_t
kernel_expected_entry(uint64 va)
{
  uint64 high_size = kernel_high_ram_size();
  uint64 text_end = (uint64)kernel_text_end;
  uint64 rodata_end = (uint64)kernel_rodata_end;
  uint64 permissions;

  if (va >= KERNEL_LOAD && va < text_end)
    permissions = PTE_R | PTE_X;
  else if (va >= text_end && va < rodata_end)
    permissions = PTE_R;
  else if (va >= rodata_end && va < LOW_RAM_END)
    permissions = PTE_R | PTE_W;
  else if (va >= BARE_SEGMENT_BACKING_BASE &&
           va < BARE_SEGMENT_BACKING_LIMIT)
    permissions = PTE_R | PTE_W;
  else if (va >= PHYSICAL_HIGH_RAM_BASE &&
           va - PHYSICAL_HIGH_RAM_BASE < high_size)
    permissions = PTE_R | PTE_W;
  else if (kernel_device_page(va))
    permissions = PTE_R | PTE_W;
  else
    return 0;

  return mmix_pte_make(va, MMIX_KERNEL_N, permissions);
}

static int
level1_child_required(uint64 index)
{
  uint64 high_size = kernel_high_ram_size();
  uint64 base;
  uint64 limit;

  if (index == 0)
    return 0;
  base = index * MMIX_PT_LEVEL1_SPAN;
  limit = base + MMIX_PT_LEVEL1_SPAN;
  return (base < LOW_RAM_END && limit > KERNEL_LOAD) ||
         (base < BARE_SEGMENT_BACKING_LIMIT &&
          limit > BARE_SEGMENT_BACKING_BASE) ||
         ranges_overlap(base, MMIX_PT_LEVEL1_SPAN, PHYSICAL_HIGH_RAM_BASE,
                        high_size) ||
         kernel_device_span(base, MMIX_PT_LEVEL1_SPAN);
}

static uint
kernel_child_tables(void)
{
  uint count = 0;

  for (uint index = 1; index < MMIX_PT_ENTRIES; index++)
    if (level1_child_required(index))
      count++;
  return count;
}

static int
child_pointer_valid(pagetable_t pagetable, pte_t pointer, pte_t *root,
                    uint index)
{
  if (!ptp_valid(pagetable, pointer))
    return -1;
  for (uint prior = 0; prior < index; prior++)
    if (root[prior] == pointer)
      return -1;
  return 0;
}

static int
require_identity(pagetable_t pagetable, uint64 va, uint64 permissions)
{
  uint64 pa;

  return mmix_pagetable_translate(pagetable, va, permissions, &pa) == 0 &&
             pa == va
           ? 0
           : -1;
}

static int
require_unmapped(pagetable_t pagetable, uint64 va)
{
  uint64 pa;

  return mmix_pagetable_translate(pagetable, va, PTE_R, &pa) < 0 ? 0 : -1;
}

static int
require_identity_permissions(pagetable_t pagetable, uint64 va,
                             uint64 permissions)
{
  uint64 pa;
  pte_t *leaf;

  return mmix_pagetable_translate(pagetable, va, permissions, &pa) == 0 &&
             pa == va && walk_leaf(pagetable, va, 0, &leaf) == WALK_OK &&
             mmix_pte_permissions(*leaf) == permissions
           ? 0
           : -1;
}

static int
kernel_pagetable_audit(pagetable_t pagetable)
{
  uint child_count = 0;
  uint expected_children;
  uint64 high_size = kernel_high_ram_size();
  uint64 root_pa;
  pte_t *direct_root;
  pte_t *level1_root;
  pte_t *level2_root;
  uint64 ro;
  uint64 rs;
  uint64 first_free = KALLOC_START((uint64)kernel_end);

  if (!pagetable_valid(pagetable) || !kernel_physical_memory_valid())
    return -1;
  expected_children = kernel_child_tables();
  root_pa = MMIX_RV_ROOT_PA(pagetable->rv);
  direct_root = table_address(root_pa);
  level1_root = table_address(root_pa + PGSIZE);
  level2_root = table_address(root_pa + 2 * PGSIZE);

  for (uint index = 0; index < MMIX_PT_ENTRIES; index++) {
    uint64 va = (uint64)index * PGSIZE;
    if (direct_root[index] != kernel_expected_entry(va))
      return -1;
  }

  for (uint index = 0; index < MMIX_PT_ENTRIES; index++) {
    pte_t pointer = level1_root[index];

    if (!level1_child_required(index)) {
      if (pointer != 0)
        return -1;
      continue;
    }
    if (child_pointer_valid(pagetable, pointer, level1_root, index) < 0)
      return -1;
    uint64 child_pa = mmix_ptp_child_pa(pointer);
    uint64 alias = mmix_phys_alias(child_pa);
    pte_t *child = table_address(child_pa);

    if ((alias & MMIX_PHYSICAL_ALIAS_BIT) == 0 ||
        (alias & ~MMIX_PHYSICAL_ALIAS_BIT) != child_pa ||
        require_identity(pagetable, child_pa, PTE_R | PTE_W) < 0 ||
        require_unmapped(pagetable, alias) < 0)
      return -1;
    child_count++;
    for (uint leaf = 0; leaf < MMIX_PT_ENTRIES; leaf++) {
      uint64 va = index * MMIX_PT_LEVEL1_SPAN + (uint64)leaf * PGSIZE;
      if (child[leaf] != kernel_expected_entry(va))
        return -1;
    }
  }

  for (uint index = 0; index < MMIX_PT_ENTRIES; index++)
    if (level2_root[index] != 0)
      return -1;
  if (child_count != expected_children)
    return -1;

  ro = mmix_ro_read();
  rs = mmix_rs_read();
  if (!platform_cpu_initial_stack_contains(cpuid(), ro) ||
      !platform_cpu_initial_stack_contains(cpuid(), rs) ||
      require_identity(pagetable, ro, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, rs, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, (uint64)kvminit, PTE_R | PTE_X) < 0 ||
      require_identity(pagetable, (uint64)kvminithart, PTE_R | PTE_X) < 0 ||
      require_identity(pagetable, (uint64)&child_count, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, (uint64)&mmix_boot_handoffs[0],
                       PTE_R | PTE_W) < 0 ||
      require_identity(pagetable,
                       (uint64)&mmix_boot_handoffs[MMIX_MAX_CPUS - 1],
                       PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, (uint64)&mmix_startup, PTE_R | PTE_W) < 0 ||
      require_identity(
        pagetable, (uint64)&mmix_startup.cpu_stage[MMIX_MAX_CPUS - 1],
        PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, (uint64)&kernel_pagetable, PTE_R | PTE_W) <
        0 ||
      require_identity(pagetable, first_free, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, KALLOC_LOW_LIMIT - PGSIZE,
                       PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, (uint64)kernel_boot_stacks_start,
                       PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, (uint64)kernel_boot_stacks_end - PGSIZE,
                       PTE_R | PTE_W) < 0 ||
      require_identity_permissions(pagetable, POOL_PHYS_BASE,
                                   PTE_R | PTE_W) < 0 ||
      require_identity_permissions(pagetable, POOL_PHYS_END - PGSIZE,
                                   PTE_R | PTE_W) < 0 ||
      require_identity_permissions(pagetable, DATA_PHYS_BASE,
                                   PTE_R | PTE_W) < 0 ||
      require_identity_permissions(pagetable, DATA_PHYS_END - PGSIZE,
                                   PTE_R | PTE_W) < 0 ||
      require_identity_permissions(pagetable, STACK_PHYS_BASE,
                                   PTE_R | PTE_W) < 0 ||
      require_identity_permissions(pagetable, STACK_PHYS_END - PGSIZE,
                                   PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, UART0_BASE, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, VIRTIO0_BASE, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, FRAMEBUFFER_CONTROL_BASE,
                       PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, TIMER_BASE, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, INTC_BASE + INTC_SIZE - 1,
                       PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, IPI_BASE + IPI_SIZE - 1,
                       PTE_R | PTE_W) < 0)
    return -1;

  if (require_identity(pagetable, KERNEL_LOAD, PTE_R | PTE_X) < 0 ||
      mmix_pagetable_translate(pagetable, KERNEL_LOAD, PTE_W, &root_pa) == 0 ||
      ((uint64)kernel_text_end < (uint64)kernel_rodata_end &&
       (require_identity(pagetable, (uint64)kernel_text_end, PTE_R) < 0 ||
        mmix_pagetable_translate(pagetable, (uint64)kernel_text_end, PTE_W,
                                 &root_pa) == 0)) ||
      require_identity(pagetable, (uint64)kernel_rodata_end, PTE_R | PTE_W) <
        0 ||
      mmix_pagetable_translate(pagetable, (uint64)kernel_rodata_end, PTE_X,
                               &root_pa) == 0 ||
      mmix_pagetable_translate(pagetable, UART0_BASE, PTE_X, &root_pa) == 0)
    return -1;

  if (require_unmapped(pagetable, MMIX_LOW_VECTOR_BASE) < 0 ||
      require_unmapped(pagetable, KERNEL_ROOT_BASE) < 0 ||
      require_unmapped(pagetable, PLATFORM_RAM_BASE) < 0 ||
      require_unmapped(pagetable, PLATFORM_RAM_END - PGSIZE) < 0 ||
      require_unmapped(pagetable, FRAMEBUFFER_BASE) < 0 ||
      require_unmapped(pagetable, FRAMEBUFFER_END - PGSIZE) < 0 ||
      require_unmapped(pagetable, MMIO_DEVICE_PAGE_END) < 0 ||
      require_unmapped(pagetable,
                       MMIO_APERTURE_BASE + MMIO_APERTURE_SIZE / 2) < 0 ||
      require_unmapped(pagetable, MMIO_APERTURE_END - PGSIZE) < 0 ||
      require_unmapped(pagetable, MMIX_SEGMENT0_LIMIT - PGSIZE) < 0 ||
      require_unmapped(pagetable, POOL_LOGICAL_BASE) < 0 ||
      require_unmapped(pagetable, DATA_LOGICAL_BASE) < 0 ||
      require_unmapped(pagetable, STACK_LOGICAL_BASE) < 0)
    return -1;

  if (high_size == 0) {
    if (require_unmapped(pagetable, PHYSICAL_HIGH_RAM_BASE) < 0)
      return -1;
  } else {
    uint64 middle = PHYSICAL_HIGH_RAM_BASE +
                    ROUNDDOWN(high_size / 2, PGSIZE);
    uint64 alias = mmix_phys_alias(middle);

    if (require_identity_permissions(pagetable, PHYSICAL_HIGH_RAM_BASE,
                                     PTE_R | PTE_W) < 0 ||
        require_identity_permissions(pagetable, middle, PTE_R | PTE_W) < 0 ||
        require_identity_permissions(pagetable,
                                     PHYSICAL_HIGH_RAM_BASE + high_size - PGSIZE,
                                     PTE_R | PTE_W) < 0 ||
        (alias & MMIX_PHYSICAL_ALIAS_BIT) == 0 ||
        (alias & ~MMIX_PHYSICAL_ALIAS_BIT) != middle ||
        require_unmapped(pagetable, alias) < 0)
      return -1;
  }

  return 0;
}

static int
map_kernel_devices(void)
{
  for (uint64 va = MMIO_APERTURE_BASE; va < MMIO_DEVICE_PAGE_END;
       va += PGSIZE)
    if (kernel_device_page(va) &&
        mappages(kernel_pagetable, va, PGSIZE, va, PTE_R | PTE_W) < 0)
      return -1;
  return 0;
}

void
kvminit(void)
{
  uint64 text_end = (uint64)kernel_text_end;
  uint64 rodata_end = (uint64)kernel_rodata_end;
  uint64 high_size = kernel_high_ram_size();
  uint expected_children;
  uint64 free_before = kalloc_free_pages();
  uint64 free_after;

  if (mmix_rv_read() == MMIX_KERNEL_RV ||
      KERNEL_ROOT_BASE != MMIX_LOW_VECTOR_LIMIT ||
      (KERNEL_ROOT_BASE & (PGSIZE - 1)) != 0 ||
      KERNEL_ROOT_LIMIT - KERNEL_ROOT_BASE !=
        MMIX_KERNEL_ROOT_BLOCKS * PGSIZE ||
      KERNEL_ROOT_LIMIT > KERNEL_LOAD || text_end <= KERNEL_LOAD ||
      (text_end & (PGSIZE - 1)) != 0 || rodata_end < text_end ||
      (rodata_end & (PGSIZE - 1)) != 0 || rodata_end > (uint64)kernel_end ||
      (uint64)kernel_end > KALLOC_LOW_LIMIT ||
      !kernel_physical_memory_valid())
    panic("kvminit layout");
  expected_children = kernel_child_tables();

  kernel_table.rv = MMIX_KERNEL_RV;
  memset(table_address(KERNEL_ROOT_BASE), 0, KERNEL_ROOT_SIZE);

  if (mappages(kernel_pagetable, KERNEL_LOAD, text_end - KERNEL_LOAD,
               KERNEL_LOAD, PTE_R | PTE_X) < 0 ||
      (rodata_end > text_end &&
       mappages(kernel_pagetable, text_end, rodata_end - text_end, text_end,
                PTE_R) < 0) ||
      mappages(kernel_pagetable, rodata_end, LOW_RAM_END - rodata_end,
               rodata_end, PTE_R | PTE_W) < 0 ||
      mappages(kernel_pagetable, BARE_SEGMENT_BACKING_BASE,
               BARE_SEGMENT_BACKING_SIZE, BARE_SEGMENT_BACKING_BASE,
               PTE_R | PTE_W) < 0 ||
      (high_size != 0 &&
       mappages(kernel_pagetable, PHYSICAL_HIGH_RAM_BASE, high_size,
                PHYSICAL_HIGH_RAM_BASE,
                PTE_R | PTE_W) < 0) ||
      map_kernel_devices() < 0)
    panic("kvminit map");

  free_after = kalloc_free_pages();
  if (free_before < free_after ||
      free_before - free_after != expected_children)
    panic("kvminit tables");
  if (kernel_pagetable_audit(kernel_pagetable) < 0)
    panic("kvminit audit");
}

void
kvminithart(void)
{
  uint64 cpu_id = cpuid();
  if (mmix_rv_read() == kernel_pagetable->rv || mmix_intr_get())
    panic("paging state");
  mmix_rv_publish(kernel_pagetable->rv);
  if (mmix_rv_read() != kernel_pagetable->rv || mmix_intr_get() ||
      !platform_cpu_initial_stack_contains(cpu_id, mmix_ro_read()) ||
      !platform_cpu_initial_stack_contains(cpu_id, mmix_rs_read()))
    panic("paging enable");
}
