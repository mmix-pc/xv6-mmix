#ifndef XV6_MMIX_H
#define XV6_MMIX_H

#include "memlayout.h"

// xv6 page interface.
#define PGSIZE             MMIX_PAGE_SIZE
#define PGSHIFT            13
#define PGROUNDUP(value)   (((value) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(value) ((value) & ~(PGSIZE - 1))

// MMIX leaf permissions are pr:pw:px in bits 2:0. There is no valid or user
// bit; an entry without the permission required by an access faults.
#define PTE_X (1 << 0)
#define PTE_W (1 << 1)
#define PTE_R (1 << 2)

// MMIX translation format.
// Hardware translation uses radix-1024 blocks of 1024 octas.
#define MMIX_PT_INDEX_BITS 10
#define MMIX_PT_ENTRIES    (1 << MMIX_PT_INDEX_BITS)
#define MMIX_PT_INDEX_MASK (MMIX_PT_ENTRIES - 1)

#define MMIX_VA_SEGMENT_SHIFT       61
#define MMIX_VA_SEGMENT_MASK        0x7
#define MMIX_VA_SEGMENT_OFFSET_MASK 0x1fffffffffffffff

#define MMIX_RV_B1_SHIFT     60
#define MMIX_RV_B2_SHIFT     56
#define MMIX_RV_B3_SHIFT     52
#define MMIX_RV_B4_SHIFT     48
#define MMIX_RV_B_VALUE_MASK 0xf
#define MMIX_RV_S_SHIFT      40
#define MMIX_RV_S_VALUE_MASK 0xff
#define MMIX_RV_R_SHIFT      13
#define MMIX_RV_R_VALUE_MASK 0x7ffffff
#define MMIX_RV_N_SHIFT      3
#define MMIX_RV_N_VALUE_MASK 0x3ff
#define MMIX_RV_F_VALUE_MASK 0x7
#define MMIX_RV_F_HARDWARE   0

#define MMIX_RV_B1(value) (((value) >> MMIX_RV_B1_SHIFT) & MMIX_RV_B_VALUE_MASK)
#define MMIX_RV_B2(value) (((value) >> MMIX_RV_B2_SHIFT) & MMIX_RV_B_VALUE_MASK)
#define MMIX_RV_B3(value) (((value) >> MMIX_RV_B3_SHIFT) & MMIX_RV_B_VALUE_MASK)
#define MMIX_RV_B4(value) (((value) >> MMIX_RV_B4_SHIFT) & MMIX_RV_B_VALUE_MASK)
#define MMIX_RV_S(value)  (((value) >> MMIX_RV_S_SHIFT) & MMIX_RV_S_VALUE_MASK)
#define MMIX_RV_R(value)  (((value) >> MMIX_RV_R_SHIFT) & MMIX_RV_R_VALUE_MASK)
#define MMIX_RV_ROOT_PA(value) (MMIX_RV_R(value) << PGSHIFT)
#define MMIX_RV_N(value) (((value) >> MMIX_RV_N_SHIFT) & MMIX_RV_N_VALUE_MASK)
#define MMIX_RV_F(value) ((value) & MMIX_RV_F_VALUE_MASK)

#define MMIX_ENTRY_N_SHIFT      3
#define MMIX_ENTRY_N_VALUE_MASK 0x3ff
#define MMIX_ENTRY_N_FIELD_MASK (MMIX_ENTRY_N_VALUE_MASK << MMIX_ENTRY_N_SHIFT)

#define MMIX_PTE_PERM_VALUE_MASK 0x7
#define MMIX_PHYS_BITS           48
#define MMIX_PHYS_VALUE_MASK     0x0000ffffffffffff
#define MMIX_PTE_PA_FIELD_MASK   0x0000ffffffffe000

#define MMIX_PTP_SIGN_BIT       0x8000000000000000
#define MMIX_PTP_C_FIELD_MASK   0x7fffffffffffe000
#define MMIX_PHYSICAL_ALIAS_BIT 0x8000000000000000

// Kernel translation configuration. b1:b2:b3:b4 = 3:2:1:0 gives segment 0
// three radix-1024 digits and makes segments 1 through 3 empty. Keep the
// complete rV literal visible so the boot ABI can be audited directly.
#define MMIX_KERNEL_B0 0
#define MMIX_KERNEL_B1 3
#define MMIX_KERNEL_B2 2
#define MMIX_KERNEL_B3 1
#define MMIX_KERNEL_B4 0
#define MMIX_KERNEL_S  PGSHIFT
#define MMIX_KERNEL_R  (KERNEL_ROOT_BASE >> PGSHIFT)
#define MMIX_KERNEL_N  0
#define MMIX_KERNEL_F  MMIX_RV_F_HARDWARE
#define MMIX_KERNEL_RV 0x32100d0000002000

#define MMIX_KERNEL_ROOT_BLOCKS (MMIX_KERNEL_B1 - MMIX_KERNEL_B0)
#define MMIX_SEGMENT0_LIMIT     0x0000080000000000

#if !defined(__ASSEMBLER__)

#include "types.h"

// C types and encoding helpers.
#define MMIX_RV_BUILD(b1, b2, b3, b4, s, r, n, f)                              \
  ((((uint64)(b1) & MMIX_RV_B_VALUE_MASK) << MMIX_RV_B1_SHIFT) |               \
   (((uint64)(b2) & MMIX_RV_B_VALUE_MASK) << MMIX_RV_B2_SHIFT) |               \
   (((uint64)(b3) & MMIX_RV_B_VALUE_MASK) << MMIX_RV_B3_SHIFT) |               \
   (((uint64)(b4) & MMIX_RV_B_VALUE_MASK) << MMIX_RV_B4_SHIFT) |               \
   (((uint64)(s) & MMIX_RV_S_VALUE_MASK) << MMIX_RV_S_SHIFT) |                 \
   (((uint64)(r) & MMIX_RV_R_VALUE_MASK) << MMIX_RV_R_SHIFT) |                 \
   (((uint64)(n) & MMIX_RV_N_VALUE_MASK) << MMIX_RV_N_SHIFT) |                 \
   ((uint64)(f) & MMIX_RV_F_VALUE_MASK))

typedef uint64 pte_t;

// MMIX page tables are described by rV, not by an Sv39-style root-page
// pointer. The root physical address, segment spans, page size, and address
// space number are all decoded from this value.
struct mmix_pagetable {
  uint64 rv;
};

typedef struct mmix_pagetable *pagetable_t;

static inline uint64
mmix_rv_make(uint64 root_pa, uint64 asn)
{
  return MMIX_RV_BUILD(MMIX_KERNEL_B1, MMIX_KERNEL_B2, MMIX_KERNEL_B3,
                       MMIX_KERNEL_B4, MMIX_KERNEL_S, root_pa >> PGSHIFT, asn,
                       MMIX_KERNEL_F);
}

static inline uint64
mmix_va_segment(uint64 va)
{
  return (va >> MMIX_VA_SEGMENT_SHIFT) & MMIX_VA_SEGMENT_MASK;
}

static inline uint64
mmix_va_page(uint64 va)
{
  return (va & MMIX_VA_SEGMENT_OFFSET_MASK) >> PGSHIFT;
}

static inline uint64
mmix_va_index(uint64 va, uint level)
{
  return (mmix_va_page(va) >> (level * MMIX_PT_INDEX_BITS)) &
         MMIX_PT_INDEX_MASK;
}

static inline uint64
mmix_phys_alias(uint64 pa)
{
  return MMIX_PHYSICAL_ALIAS_BIT | pa;
}

static inline pte_t
mmix_pte_make(uint64 pa, uint64 asn, uint64 permissions)
{
  return (pa & MMIX_PTE_PA_FIELD_MASK) |
         ((asn & MMIX_ENTRY_N_VALUE_MASK) << MMIX_ENTRY_N_SHIFT) |
         (permissions & MMIX_PTE_PERM_VALUE_MASK);
}

static inline uint64
mmix_pte_pa(pte_t pte)
{
  return pte & MMIX_PTE_PA_FIELD_MASK;
}

static inline uint64
mmix_entry_n(pte_t entry)
{
  return (entry >> MMIX_ENTRY_N_SHIFT) & MMIX_ENTRY_N_VALUE_MASK;
}

static inline uint64
mmix_pte_permissions(pte_t pte)
{
  return pte & MMIX_PTE_PERM_VALUE_MASK;
}

static inline pte_t
mmix_ptp_make(uint64 child_pa, uint64 asn)
{
  return MMIX_PTP_SIGN_BIT | (child_pa & MMIX_PTP_C_FIELD_MASK) |
         ((asn & MMIX_ENTRY_N_VALUE_MASK) << MMIX_ENTRY_N_SHIFT);
}

static inline uint64
mmix_ptp_child_pa(pte_t ptp)
{
  return ptp & MMIX_PTP_C_FIELD_MASK;
}

static inline int
mmix_ptp_matches(pte_t ptp, uint64 asn)
{
  return (ptp & MMIX_PTP_SIGN_BIT) != 0 && mmix_entry_n(ptp) == asn;
}

static inline uint64
mmix_rv_read(void)
{
  uint64 value;

  asm volatile("GET %0, rV" : "=r"(value));
  return value;
}

static inline uint64
mmix_ro_read(void)
{
  uint64 value;

  asm volatile("GET %0, rO" : "=r"(value));
  return value;
}

static inline uint64
mmix_rs_read(void)
{
  uint64 value;

  asm volatile("GET %0, rS" : "=r"(value));
  return value;
}

static inline void
mmix_rv_write(uint64 value)
{
  asm volatile("PUT rV, %0" : : "r"(value) : "memory");
}

static inline void
mmix_sync_memory(void)
{
  asm volatile("SYNC 3" : : : "memory");
}

static inline void
mmix_sync_translation(void)
{
  asm volatile("SYNC 6" : : : "memory");
}

static inline uint64
mmix_rk_read(void)
{
  uint64 value;

  asm volatile("GET %0, rK" : "=r"(value));
  return value;
}

static inline int
mmix_intr_get(void)
{
  return mmix_rk_read() != 0;
}

// The current kernel keeps dynamic interrupts disabled, so only the masking
// operation is exposed here. Trap initialization will define the eventual
// enable policy.
static inline void
mmix_intr_off(void)
{
  uint64 disabled = 0;

  asm volatile("PUT rK, %0" : : "r"(disabled) : "memory");
}

// SYNC 6 is the architectural full translation-cache invalidation. Current
// QEMU also requires rewriting rV to flush its software TLB after a live PTE
// change; the same sequence performs the initial transition out of flat mode.
static inline void
mmix_rv_publish(uint64 value)
{
  asm volatile("" : : : "memory");
  mmix_sync_memory();
  mmix_sync_translation();
  mmix_rv_write(value);
}

_Static_assert(sizeof(uint64) == 8, "MMIX octas must be 8 bytes");
_Static_assert(PGSIZE == 0x2000, "MMIX pages must be 8 KiB");
_Static_assert(MMIX_PT_ENTRIES * sizeof(pte_t) == PGSIZE,
               "one page-table block must contain 1024 octas");
_Static_assert((MMIX_PTE_PA_FIELD_MASK &
                (MMIX_ENTRY_N_FIELD_MASK | MMIX_PTE_PERM_VALUE_MASK)) == 0,
               "PTE physical and metadata fields must not overlap");
_Static_assert((MMIX_PTE_PA_FIELD_MASK | (PGSIZE - 1)) == MMIX_PHYS_VALUE_MASK,
               "PTE physical field must describe a 48-bit address");
_Static_assert((MMIX_PTP_C_FIELD_MASK & MMIX_PTP_SIGN_BIT) == 0,
               "PTP child and sign fields must not overlap");
_Static_assert((MMIX_PTP_C_FIELD_MASK &
                (MMIX_ENTRY_N_FIELD_MASK | MMIX_PTE_PERM_VALUE_MASK)) == 0,
               "PTP child and metadata fields must not overlap");
_Static_assert((MMIX_PTP_C_FIELD_MASK & (PGSIZE - 1)) == 0,
               "PTP child field must preserve page alignment");
_Static_assert(MMIX_KERNEL_RV == MMIX_RV_BUILD(MMIX_KERNEL_B1, MMIX_KERNEL_B2,
                                               MMIX_KERNEL_B3, MMIX_KERNEL_B4,
                                               MMIX_KERNEL_S, MMIX_KERNEL_R,
                                               MMIX_KERNEL_N, MMIX_KERNEL_F),
               "kernel rV literal must match its named fields");
_Static_assert(MMIX_RV_B1(MMIX_KERNEL_RV) == MMIX_KERNEL_B1 &&
                 MMIX_RV_B2(MMIX_KERNEL_RV) == MMIX_KERNEL_B2 &&
                 MMIX_RV_B3(MMIX_KERNEL_RV) == MMIX_KERNEL_B3 &&
                 MMIX_RV_B4(MMIX_KERNEL_RV) == MMIX_KERNEL_B4,
               "kernel rV segment boundaries must match the ABI");
_Static_assert(MMIX_RV_S(MMIX_KERNEL_RV) == MMIX_KERNEL_S,
               "kernel rV page size must match PGSIZE");
_Static_assert(MMIX_RV_ROOT_PA(MMIX_KERNEL_RV) == KERNEL_ROOT_BASE,
               "kernel rV root must match the reserved physical blocks");
_Static_assert(MMIX_RV_N(MMIX_KERNEL_RV) == MMIX_KERNEL_N,
               "kernel rV address-space number must match the ABI");
_Static_assert(MMIX_RV_F(MMIX_KERNEL_RV) == MMIX_KERNEL_F,
               "kernel rV must select hardware translation");
_Static_assert(KERNEL_ROOT_BLOCKS == MMIX_KERNEL_ROOT_BLOCKS,
               "segment-0 root block count must match b1 - b0");
_Static_assert(MMIX_SEGMENT0_LIMIT ==
                 (1L << (PGSHIFT + MMIX_PT_INDEX_BITS * MMIX_KERNEL_B1)),
               "segment-0 limit must match the configured table span");

#endif // !__ASSEMBLER__

#endif // XV6_MMIX_H
