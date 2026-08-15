#include "mmix.h"
#include "defs.h"
#include "kalloc.h"
#include "vm.h"

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
  return pagetable != 0 && pagetable->rv == MMIX_KERNEL_RV;
}

static int
permissions_valid(uint64 permissions)
{
  if (permissions == 0 || (permissions & ~MMIX_PTE_PERM_VALUE_MASK) != 0 ||
      (permissions & PTE_R) == 0)
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
         permissions_valid(permissions);
}

static int
walk_leaf(pagetable_t pagetable, uint64 va, int alloc, pte_t **leaf)
{
  uint64 page;
  uint64 digits[MMIX_KERNEL_B1];
  uint64 highest;
  uint64 table_pa;
  uint64 asn;

  if (!pagetable_valid(pagetable) || mmix_va_segment(va) != 0 ||
      va >= MMIX_SEGMENT0_LIMIT)
    return WALK_INVALID;

  page = mmix_va_page(va);
  for (uint level = 0; level < MMIX_KERNEL_B1; level++)
    digits[level] = (page >> (level * MMIX_PT_INDEX_BITS)) & MMIX_PT_INDEX_MASK;

  // The highest nonzero digit selects its contiguous root block. Lower
  // nonzero levels are reached through allocator-owned PTP blocks.
  highest = digits[2] != 0 ? 2 : digits[1] != 0 ? 1 : 0;
  table_pa = MMIX_RV_ROOT_PA(pagetable->rv) + highest * PGSIZE;
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
  if (!pagetable_valid(pagetable) || mmix_va_segment(va) != 0 ||
      last_va >= MMIX_SEGMENT0_LIMIT || last_pa > MMIX_PTE_PA_FIELD_MASK)
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
  if (!pagetable_valid(pagetable) || mmix_va_segment(va) != 0 ||
      (npages != 0 && va + size - PGSIZE >= MMIX_SEGMENT0_LIMIT))
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

  if (!pagetable_valid(pagetable))
    return -1;
  root_pa = MMIX_RV_ROOT_PA(pagetable->rv);
  for (uint level = 0; level < MMIX_KERNEL_ROOT_BLOCKS; level++) {
    if (free_table(pagetable, root_pa + level * PGSIZE, level) < 0) {
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
