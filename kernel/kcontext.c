#include "param.h"
#include "memlayout.h"
#include "mmix.h"
#include "defs.h"
#include "kalloc.h"
#include "vm.h"
#include "spinlock.h"
#include "proc.h"
#include "kcontext.h"

enum {
  MMIX_CONTEXT_MAPPED_PAGES = 2,
  MMIX_CONTEXT_NEW_TABLE_PAGES = 2,
};

static uint64 context_software_pa[MMIX_CONTEXT_SLOT_COUNT];
static uint64 context_register_pa[MMIX_CONTEXT_SLOT_COUNT];

_Static_assert(__builtin_offsetof(struct context, state) ==
                 MMIX_CONTEXT_STATE_OFFSET,
               "MMIX context state offset mismatch");
_Static_assert(sizeof(struct context) == MMIX_CONTEXT_SIZE,
               "MMIX context size mismatch");
_Static_assert(__alignof__(struct context) == MMIX_CONTEXT_ALIGN,
               "MMIX context alignment mismatch");

static int
context_unmapped(uint64 va)
{
  pte_t *pte = walk(kernel_pagetable, va, 0);

  return pte == 0 || *pte == 0;
}

static int
context_mapping_matches(uint64 va, uint64 pa)
{
  pte_t *pte = walk(kernel_pagetable, va, 0);

  return pte != 0 && *pte == mmix_pte_make(pa, MMIX_KERNEL_N, PTE_R | PTE_W) &&
         walkaddr(kernel_pagetable, va) == pa;
}

static void
context_map_page(uint64 va, uint64 *saved_pa)
{
  void *page = kalloc();

  if (page == 0)
    panic("context alloc");
  memset(page, 0, PGSIZE);
  if (mappages(kernel_pagetable, va, PGSIZE, (uint64)page, PTE_R | PTE_W) < 0)
    panic("context map");
  *saved_pa = (uint64)page;
}

void
mmix_kcontext_init(void)
{
  uint64 free_before = kalloc_free_pages();
  uint64 free_after;

  if (MMIX_CONTEXT_SLOT_COUNT != NPROC + NCPU ||
      MMIX_CONTEXT_SCHEDULER_SLOT != 0 ||
      MMIX_CONTEXT_PROCESS_SLOT_BASE != NCPU ||
      MMIX_CONTEXT_AREA_TOP != MMIX_SEGMENT0_LIMIT)
    panic("context layout");

  for (uint slot = 0; slot < MMIX_CONTEXT_SLOT_COUNT; slot++) {
    uint64 low = MMIX_CONTEXT_LOW_GUARD(slot);
    uint64 software = MMIX_CONTEXT_SOFTWARE_STACK_BASE(slot);
    uint64 middle = MMIX_CONTEXT_MIDDLE_GUARD(slot);
    uint64 registers = MMIX_CONTEXT_REGISTER_STACK_BASE(slot);
    uint64 high = MMIX_CONTEXT_HIGH_GUARD(slot);

    if (low < MMIX_CONTEXT_AREA_BASE || high >= MMIX_CONTEXT_AREA_TOP ||
        software + PGSIZE != middle || middle + PGSIZE != registers ||
        registers + PGSIZE != high || !context_unmapped(low) ||
        !context_unmapped(software) || !context_unmapped(middle) ||
        !context_unmapped(registers) || !context_unmapped(high))
      panic("context slot");

    context_map_page(software, &context_software_pa[slot]);
    context_map_page(registers, &context_register_pa[slot]);

    if (!context_mapping_matches(software, context_software_pa[slot]) ||
        !context_mapping_matches(registers, context_register_pa[slot]) ||
        !context_unmapped(low) || !context_unmapped(middle) ||
        !context_unmapped(high))
      panic("context audit");
  }

  free_after = kalloc_free_pages();
  if (free_before < free_after ||
      free_before - free_after !=
        MMIX_CONTEXT_SLOT_COUNT * MMIX_CONTEXT_MAPPED_PAGES +
          MMIX_CONTEXT_NEW_TABLE_PAGES)
    panic("context pages");
}
