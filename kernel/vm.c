#include "mmix.h"
#include "defs.h"
#include "early_print.h"
#include "kalloc.h"
#include "vm.h"

#define MMIX_PT_LEVEL1_SPAN         (PGSIZE * MMIX_PT_ENTRIES)
#define MMIX_KERNEL_RAM_CHILDREN    (LOW_RAM_END / MMIX_PT_LEVEL1_SPAN - 1)
#define MMIX_KERNEL_DEVICE_CHILDREN 1
#define MMIX_KERNEL_CHILD_TABLES                                               \
  (MMIX_KERNEL_RAM_CHILDREN + MMIX_KERNEL_DEVICE_CHILDREN)
#define MMIX_KERNEL_DEVICE_MAP_END (INTC_BASE + INTC_SIZE)
#define MMIX_KERNEL_LIVE_TEST_VA   MMIX_KERNEL_DEVICE_MAP_END
#define MMIX_KERNEL_LIVE_TEST_A    0x4d4d495850414731
#define MMIX_KERNEL_LIVE_TEST_B    0x4d4d495850414732

extern char kernel_text_end[];
extern char kernel_rodata_end[];
extern char kernel_end[];
struct mmix_boot_state;
extern struct mmix_boot_state mmix_boot;

static struct mmix_pagetable kernel_table;
pagetable_t kernel_pagetable = &kernel_table;

_Static_assert((LOW_RAM_END % MMIX_PT_LEVEL1_SPAN) == 0,
               "Low RAM must end on a level-1 table span");
_Static_assert(UART0_BASE / MMIX_PT_LEVEL1_SPAN ==
                 (MMIX_KERNEL_DEVICE_MAP_END - 1) / MMIX_PT_LEVEL1_SPAN,
               "kernel devices must share one level-1 child table");

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
         kalloc_page_is_managed((void *)root_pa) &&
         kalloc_page_is_managed((void *)(root_pa + PGSIZE)) &&
         asn >= MMIX_USER_ASN_FIRST && asn <= MMIX_USER_ASN_LAST &&
         pagetable->rv == mmix_user_rv_make(root_pa, asn);
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
mmix_unmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
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
      uint64 pa = mmix_pte_pa(*leaf);
      if (!kalloc_page_is_managed((void *)pa)) {
        if (changed)
          mmix_pagetable_sync(pagetable);
        return -1;
      }
      kfree((void *)pa);
    }
    *leaf = 0;
    changed = 1;
  }

  if (changed)
    mmix_pagetable_sync(pagetable);
  return 0;
}

void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  if (mmix_unmap(pagetable, va, npages, do_free) < 0)
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
  if (mapped != 0 && mmix_unmap(pagetable, first, mapped, 1) < 0)
    panic("uvmalloc rollback");
  return 0;
}

uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
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
                                  (last - first) / PGSIZE, 1) < 0)
    panic("uvmdealloc");
  return newsz;
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
      mmix_unmap(pagetable, start, (current - start) / PGSIZE, 1) < 0)
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
                   1) < 0)
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
      mmix_unmap(pagetable, page, 1, 1) < 0)
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
      mmix_unmap(pagetable, start, (end - start) / PGSIZE, 1) < 0)
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
                    MMIX_USER_REGISTER_STACK_TOP) == 0)
    return 0;

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

    if (mmix_pagetable_translate(pagetable, dstva, PTE_W, &pa) < 0)
      return -1;
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

    if (mmix_pagetable_translate(pagetable, srcva, PTE_R, &pa) < 0)
      return -1;
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

    if (mmix_pagetable_translate(pagetable, srcva, PTE_R, &pa) < 0)
      return -1;
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

static pte_t
kernel_expected_entry(uint64 va)
{
  uint64 text_end = (uint64)kernel_text_end;
  uint64 rodata_end = (uint64)kernel_rodata_end;
  uint64 permissions;

  if (va >= REGISTER_STACK_BASE && va < KERNEL_LOAD)
    permissions = PTE_R | PTE_W;
  else if (va >= KERNEL_LOAD && va < text_end)
    permissions = PTE_R | PTE_X;
  else if (va >= text_end && va < rodata_end)
    permissions = PTE_R;
  else if (va >= rodata_end && va < LOW_RAM_END)
    permissions = PTE_R | PTE_W;
  else if (va >= UART0_BASE && va < MMIX_KERNEL_DEVICE_MAP_END)
    permissions = PTE_R | PTE_W;
  else
    return 0;

  return mmix_pte_make(va, MMIX_KERNEL_N, permissions);
}

static int
level1_child_required(uint64 index)
{
  uint64 base;
  uint64 limit;

  if (index == 0)
    return 0;
  base = index * MMIX_PT_LEVEL1_SPAN;
  limit = base + MMIX_PT_LEVEL1_SPAN;
  return (base < LOW_RAM_END && limit > REGISTER_STACK_BASE) ||
         (base < MMIX_KERNEL_DEVICE_MAP_END && limit > UART0_BASE);
}

static int
record_child(pagetable_t pagetable, pte_t pointer, uint64 *children,
             uint *count)
{
  uint64 child_pa;

  if (!ptp_valid(pagetable, pointer) || *count >= MMIX_KERNEL_CHILD_TABLES)
    return -1;
  child_pa = mmix_ptp_child_pa(pointer);
  for (uint index = 0; index < *count; index++)
    if (children[index] == child_pa)
      return -1;
  children[(*count)++] = child_pa;
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
kernel_pagetable_audit(pagetable_t pagetable)
{
  uint64 children[MMIX_KERNEL_CHILD_TABLES];
  uint child_count = 0;
  uint64 root_pa;
  pte_t *direct_root;
  pte_t *level1_root;
  pte_t *level2_root;
  uint64 ro;
  uint64 rs;
  uint64 first_free = KALLOC_START((uint64)kernel_end);

  if (!pagetable_valid(pagetable))
    return -1;
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
    if (record_child(pagetable, pointer, children, &child_count) < 0)
      return -1;
    pte_t *child = table_address(mmix_ptp_child_pa(pointer));
    for (uint leaf = 0; leaf < MMIX_PT_ENTRIES; leaf++) {
      uint64 va = index * MMIX_PT_LEVEL1_SPAN + (uint64)leaf * PGSIZE;
      if (child[leaf] != kernel_expected_entry(va))
        return -1;
    }
  }

  for (uint index = 0; index < MMIX_PT_ENTRIES; index++)
    if (level2_root[index] != 0)
      return -1;
  if (child_count != MMIX_KERNEL_CHILD_TABLES)
    return -1;

  for (uint index = 0; index < child_count; index++) {
    uint64 alias = mmix_phys_alias(children[index]);
    if ((alias & MMIX_PHYSICAL_ALIAS_BIT) == 0 ||
        (alias & ~MMIX_PHYSICAL_ALIAS_BIT) != children[index] ||
        require_identity(pagetable, children[index], PTE_R | PTE_W) < 0 ||
        require_unmapped(pagetable, alias) < 0)
      return -1;
  }

  ro = mmix_ro_read();
  rs = mmix_rs_read();
  if (ro < REGISTER_STACK_BASE || ro >= REGISTER_STACK_LIMIT ||
      rs < REGISTER_STACK_BASE || rs >= REGISTER_STACK_LIMIT ||
      require_identity(pagetable, ro, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, rs, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, (uint64)kvminit, PTE_R | PTE_X) < 0 ||
      require_identity(pagetable, (uint64)kvminithart, PTE_R | PTE_X) < 0 ||
      require_identity(pagetable, (uint64)&children, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, (uint64)&mmix_boot, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, (uint64)&kernel_pagetable, PTE_R | PTE_W) <
        0 ||
      require_identity(pagetable, first_free, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, KALLOC_LIMIT - PGSIZE, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, BOOT_STACK_BASE, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, UART0_BASE, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, VIRTIO0_BASE, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, TIMER_BASE, PTE_R | PTE_W) < 0 ||
      require_identity(pagetable, INTC_BASE + INTC_SIZE - 1, PTE_R | PTE_W) < 0)
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
      require_unmapped(pagetable, POOL_PHYS_BASE) < 0 ||
      require_unmapped(pagetable, BOOTINFO_BASE) < 0 ||
      require_unmapped(pagetable, FRAMEBUFFER_BASE) < 0 ||
      require_unmapped(pagetable, MMIX_KERNEL_DEVICE_MAP_END) < 0 ||
      require_unmapped(pagetable, MMIX_SEGMENT0_LIMIT - PGSIZE) < 0 ||
      require_unmapped(pagetable, DATA_LOGICAL_BASE) < 0)
    return -1;

  return 0;
}

void
kvminit(void)
{
  uint64 text_end = (uint64)kernel_text_end;
  uint64 rodata_end = (uint64)kernel_rodata_end;
  uint64 free_before = kalloc_free_pages();
  uint64 free_after;

  if (mmix_rv_read() == MMIX_KERNEL_RV ||
      KERNEL_ROOT_BASE != MMIX_LOW_VECTOR_LIMIT ||
      (KERNEL_ROOT_BASE & (PGSIZE - 1)) != 0 ||
      KERNEL_ROOT_LIMIT - KERNEL_ROOT_BASE !=
        MMIX_KERNEL_ROOT_BLOCKS * PGSIZE ||
      KERNEL_ROOT_LIMIT > REGISTER_STACK_BASE || text_end <= KERNEL_LOAD ||
      (text_end & (PGSIZE - 1)) != 0 || rodata_end < text_end ||
      (rodata_end & (PGSIZE - 1)) != 0 || rodata_end > (uint64)kernel_end ||
      (uint64)kernel_end > KALLOC_LIMIT)
    panic("kvminit layout");

  kernel_table.rv = MMIX_KERNEL_RV;
  memset(table_address(KERNEL_ROOT_BASE), 0, KERNEL_ROOT_SIZE);

  if (mappages(kernel_pagetable, REGISTER_STACK_BASE,
               KERNEL_LOAD - REGISTER_STACK_BASE, REGISTER_STACK_BASE,
               PTE_R | PTE_W) < 0 ||
      mappages(kernel_pagetable, KERNEL_LOAD, text_end - KERNEL_LOAD,
               KERNEL_LOAD, PTE_R | PTE_X) < 0 ||
      (rodata_end > text_end &&
       mappages(kernel_pagetable, text_end, rodata_end - text_end, text_end,
                PTE_R) < 0) ||
      mappages(kernel_pagetable, rodata_end, LOW_RAM_END - rodata_end,
               rodata_end, PTE_R | PTE_W) < 0 ||
      mappages(kernel_pagetable, UART0_BASE,
               MMIX_KERNEL_DEVICE_MAP_END - UART0_BASE, UART0_BASE,
               PTE_R | PTE_W) < 0)
    panic("kvminit map");

  free_after = kalloc_free_pages();
  if (free_before < free_after ||
      free_before - free_after != MMIX_KERNEL_CHILD_TABLES)
    panic("kvminit tables");
  if (kernel_pagetable_audit(kernel_pagetable) < 0)
    panic("kvminit audit");
}

// FIXME: Remove when normal VM lifecycle tests exercise live MMIX PTE updates
// and TLB synchronization without relying on this early boot check.
static void
kernel_pagetable_live_audit(void)
{
  volatile uint64 *first;
  volatile uint64 *second;
  volatile uint64 *test = (volatile uint64 *)MMIX_KERNEL_LIVE_TEST_VA;
  uint64 free_before = kalloc_free_pages();
  pte_t *leaf;

  first = kalloc();
  second = kalloc();
  if (first == 0 || second == 0)
    panic("paging alloc");
  *first = MMIX_KERNEL_LIVE_TEST_A;
  *second = MMIX_KERNEL_LIVE_TEST_B;

  if (mappages(kernel_pagetable, MMIX_KERNEL_LIVE_TEST_VA, PGSIZE,
               (uint64)first, PTE_R | PTE_W) < 0 ||
      *test != MMIX_KERNEL_LIVE_TEST_A)
    panic("paging map");
  *test = ~MMIX_KERNEL_LIVE_TEST_A;
  if (*first != ~MMIX_KERNEL_LIVE_TEST_A)
    panic("paging write");

  leaf = walk(kernel_pagetable, MMIX_KERNEL_LIVE_TEST_VA, 0);
  if (leaf == 0 ||
      *leaf != mmix_pte_make((uint64)first, MMIX_KERNEL_N, PTE_R | PTE_W))
    panic("paging leaf");
  *leaf = mmix_pte_make((uint64)second, MMIX_KERNEL_N, PTE_R | PTE_W);
  mmix_pagetable_sync(kernel_pagetable);
  if (*test != MMIX_KERNEL_LIVE_TEST_B)
    panic("paging update");

  uvmunmap(kernel_pagetable, MMIX_KERNEL_LIVE_TEST_VA, 1, 0);
  if (walkaddr(kernel_pagetable, MMIX_KERNEL_LIVE_TEST_VA) != 0)
    panic("paging unmap");
  kfree((void *)second);
  kfree((void *)first);
  if (kalloc_free_pages() != free_before)
    panic("paging restore");
}

void
kvminithart(void)
{
  volatile uint64 stack_probe = MMIX_KERNEL_LIVE_TEST_A;

  if (mmix_rv_read() == kernel_pagetable->rv || mmix_intr_get())
    panic("paging state");
  mmix_rv_publish(kernel_pagetable->rv);
  if (mmix_rv_read() != kernel_pagetable->rv || mmix_intr_get() ||
      stack_probe != MMIX_KERNEL_LIVE_TEST_A ||
      mmix_ro_read() < REGISTER_STACK_BASE ||
      mmix_ro_read() >= REGISTER_STACK_LIMIT ||
      mmix_rs_read() < REGISTER_STACK_BASE ||
      mmix_rs_read() >= REGISTER_STACK_LIMIT)
    panic("paging enable");

  kernel_pagetable_live_audit();
  mmix_early_print_paging(kernel_pagetable->rv);
}
