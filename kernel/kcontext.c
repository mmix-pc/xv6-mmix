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
  MMIX_CONTEXT_GLOBAL_COUNT = 256 - MMIX_ABI_GLOBAL_FIRST,
};

struct mmix_initial_context {
  uint64 outer_hole;
  uint64 local_hole;
  uint64 globals[MMIX_CONTEXT_GLOBAL_COUNT];
  uint64 rb;
  uint64 rd;
  uint64 re;
  uint64 rh;
  uint64 rj;
  uint64 rm;
  uint64 rr;
  uint64 rp;
  uint64 rw;
  uint64 rx;
  uint64 ry;
  uint64 rz;
  uint64 rg_ra;
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
_Static_assert(__builtin_offsetof(struct mmix_initial_context, rg_ra) ==
                 MMIX_CONTEXT_INITIAL_STATE_OFFSET,
               "MMIX initial context state offset mismatch");
_Static_assert(sizeof(struct mmix_initial_context) ==
                 MMIX_CONTEXT_INITIAL_SIZE,
               "MMIX initial context size mismatch");
_Static_assert(MMIX_CONTEXT_INITIAL_STATE_OFFSET + sizeof(uint64) ==
                 MMIX_CONTEXT_INITIAL_SIZE &&
                 MMIX_CONTEXT_INITIAL_SIZE <= MMIX_PAGE_SIZE,
               "MMIX initial context must fit its register-stack page");

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
kcontext_init(void)
{
  uint64 free_before = kalloc_free_pages();
  uint64 free_after;

  if (NCPU != MMIX_MAX_CPUS ||
      MMIX_CONTEXT_PROCESS_COUNT != NPROC ||
      MMIX_CONTEXT_SLOT_COUNT != NPROC + NCPU ||
      MMIX_CONTEXT_SCHEDULER_SLOT(0) != 0 ||
      MMIX_CONTEXT_SCHEDULER_SLOT(NCPU - 1) != NCPU - 1 ||
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

static void
kcontext_prepare_internal(struct context *context, uint slot, uint64 entry,
                          uint64 arg)
{
  struct mmix_initial_context *initial;
  uint64 register_base;
  uint64 software_top;

  if (context == 0 || entry == 0 || (entry & 3) != 0 ||
      slot >= MMIX_CONTEXT_SLOT_COUNT)
    panic("context prepare");

  register_base = MMIX_CONTEXT_REGISTER_STACK_BASE(slot);
  software_top = MMIX_CONTEXT_SOFTWARE_STACK_TOP(slot);
  if (!context_mapping_matches(register_base, context_register_pa[slot]) ||
      !context_mapping_matches(MMIX_CONTEXT_SOFTWARE_STACK_BASE(slot),
                               context_software_pa[slot]))
    panic("context backing");

  initial = (struct mmix_initial_context *)register_base;
  memset(initial, 0, sizeof(*initial));
  // r231 carries the first argument into a newly prepared C context.
  initial->globals[0] = arg;
  initial->globals[MMIX_ABI_FP - MMIX_ABI_GLOBAL_FIRST] = software_top;
  initial->globals[MMIX_ABI_SP - MMIX_ABI_GLOBAL_FIRST] = software_top;
  initial->rj = entry;
  initial->rg_ra = (uint64)MMIX_ABI_GLOBAL_FIRST << 56;
  context->state = register_base + MMIX_CONTEXT_INITIAL_STATE_OFFSET;
}

void
kcontext_prepare(struct context *context, uint slot, void (*entry)(void))
{
  kcontext_prepare_internal(context, slot, (uint64)entry, 0);
}

void
kcontext_prepare_arg(struct context *context, uint slot,
                     void (*entry)(uint64), uint64 arg)
{
  kcontext_prepare_internal(context, slot, (uint64)entry, arg);
}

int
kcontext_current_valid(const struct context *context, uint slot)
{
  uint64 low;
  uint64 software;
  uint64 middle;
  uint64 registers;
  uint64 high;
  uint64 ro;
  uint64 rs;
  uint64 sp;

  if (context == 0 || slot >= MMIX_CONTEXT_SLOT_COUNT)
    return 0;
  low = MMIX_CONTEXT_LOW_GUARD(slot);
  software = MMIX_CONTEXT_SOFTWARE_STACK_BASE(slot);
  middle = MMIX_CONTEXT_MIDDLE_GUARD(slot);
  registers = MMIX_CONTEXT_REGISTER_STACK_BASE(slot);
  high = MMIX_CONTEXT_HIGH_GUARD(slot);
  ro = mmix_ro_read();
  rs = mmix_rs_read();
  sp = mmix_sp_read();

  return context->state >= registers && context->state < high &&
         sp > software && sp <= middle && ro >= registers && ro < high &&
         rs >= registers && rs < high &&
         context_mapping_matches(software, context_software_pa[slot]) &&
         context_mapping_matches(registers, context_register_pa[slot]) &&
         context_unmapped(low) && context_unmapped(middle) &&
         context_unmapped(high);
}
