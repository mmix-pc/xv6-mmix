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

// Kernel trap requests and masks. Program requests occupy rQ[39:32]; the
// QEMU virt interrupt controller drives I/O request bit 8.
#define MMIX_RQ_PROGRAM_SHIFT 32
#define MMIX_RQ_PROGRAM_R     0x0000008000000000
#define MMIX_RQ_PROGRAM_W     0x0000004000000000
#define MMIX_RQ_PROGRAM_X     0x0000002000000000
#define MMIX_RQ_PROGRAM_N     0x0000001000000000
#define MMIX_RQ_PROGRAM_K     0x0000000800000000
#define MMIX_RQ_PROGRAM_B     0x0000000400000000
#define MMIX_RQ_PROGRAM_S     0x0000000200000000
#define MMIX_RQ_PROGRAM_P     0x0000000100000000
#define MMIX_RQ_PROGRAM_MASK  0x000000ff00000000
#define MMIX_RQ_INTC          0x0000000000000100
#define MMIX_RK_INTC          MMIX_RQ_INTC

#define MMIX_KERNEL_PROGRAM_MASK                                      \
  (MMIX_RQ_PROGRAM_R | MMIX_RQ_PROGRAM_W | MMIX_RQ_PROGRAM_X |       \
   MMIX_RQ_PROGRAM_B)
#define MMIX_KERNEL_INTC_MASK MMIX_RK_INTC
#define MMIX_KERNEL_TRAP_MASK                                         \
  (MMIX_KERNEL_PROGRAM_MASK | MMIX_KERNEL_INTC_MASK)

// rA contains arithmetic event status in its low byte and the corresponding
// trip enables in the next byte. Phase 1 keeps every arithmetic trip disabled.
#define MMIX_RA_EVENT_MASK       0x00000000000000ff
#define MMIX_RA_ENABLE_SHIFT     8
#define MMIX_RA_TRIP_ENABLE_MASK 0x000000000000ff00

// A negative rXX selects the resume-next form used by forced and external
// dynamic traps.
#define MMIX_DYNAMIC_TRAP_RESUME_NEXT 0x8000000000000000

// TRAP 0,Y,Z is reserved for semihosting. The kernel uses this exact
// nonzero-X instruction when it deliberately requests a resumable trap.
#define MMIX_KERNEL_FORCED_TRAP_INSN 0x00010000

// Assembly-visible kernel trap-state layout. Global register offsets are
// computed from their architectural register numbers.
#define MMIX_TRAP_GLOBAL_FIRST 231
#define MMIX_TRAP_GLOBAL_LAST  254
#define MMIX_TRAP_GLOBAL_COUNT                                               \
  (MMIX_TRAP_GLOBAL_LAST - MMIX_TRAP_GLOBAL_FIRST + 1)
#define MMIX_TRAP_GLOBAL_OFFSET(reg)                                         \
  (((reg) - MMIX_TRAP_GLOBAL_FIRST) * 8)

#define MMIX_TRAP_RJ_OFFSET         192
#define MMIX_TRAP_RBB_OFFSET        200
#define MMIX_TRAP_RWW_OFFSET        208
#define MMIX_TRAP_RXX_OFFSET        216
#define MMIX_TRAP_RYY_OFFSET        224
#define MMIX_TRAP_RZZ_OFFSET        232
#define MMIX_TRAP_RQ_OFFSET         240
#define MMIX_TRAP_RESTORE_RK_OFFSET 248
#define MMIX_TRAP_RG_OFFSET         256
#define MMIX_TRAP_RL_OFFSET         264
#define MMIX_TRAP_RO_OFFSET         272
#define MMIX_TRAP_RS_OFFSET         280
#define MMIX_TRAP_RA_OFFSET         288
#define MMIX_TRAP_RD_OFFSET         296
#define MMIX_TRAP_RE_OFFSET         304
#define MMIX_TRAP_RH_OFFSET         312
#define MMIX_TRAP_RM_OFFSET         320
#define MMIX_TRAP_RR_OFFSET         328
#define MMIX_TRAP_RP_OFFSET         336
#define MMIX_TRAP_RF_OFFSET         344
#define MMIX_TRAP_STATE_SIZE        352
#define MMIX_TRAP_STATE_ALIGN       8

// The linked -O0 entry and deepest terminal diagnostic use less than 640
// bytes together. Keep one KiB available and re-audit this reserve whenever
// the compiler options or diagnostic call graph changes.
#define MMIX_TRAP_STACK_RESERVE 1024

#define MMIX_TRAP_VECTOR_ALIGN 16

// The entry mechanism classifies the architectural saved state before it
// crosses the C dispatch boundary.
#define MMIX_TRAP_CLASS_UNKNOWN  0
#define MMIX_TRAP_CLASS_FORCED   1
#define MMIX_TRAP_CLASS_PROGRAM  2
#define MMIX_TRAP_CLASS_EXTERNAL 3

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

struct mmix_trap_state {
  uint64 globals[MMIX_TRAP_GLOBAL_COUNT];
  uint64 rj;
  uint64 rbb;
  uint64 rww;
  uint64 rxx;
  uint64 ryy;
  uint64 rzz;
  uint64 rq;
  uint64 restore_rk;
  uint64 rg;
  uint64 rl;
  uint64 ro;
  uint64 rs;
  uint64 ra;
  uint64 rd;
  uint64 re;
  uint64 rh;
  uint64 rm;
  uint64 rr;
  uint64 rp;
  uint64 rf;
};

enum mmix_trap_class {
  MMIX_TRAP_UNKNOWN = MMIX_TRAP_CLASS_UNKNOWN,
  MMIX_TRAP_FORCED = MMIX_TRAP_CLASS_FORCED,
  MMIX_TRAP_PROGRAM = MMIX_TRAP_CLASS_PROGRAM,
  MMIX_TRAP_EXTERNAL = MMIX_TRAP_CLASS_EXTERNAL,
};

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

// Validate a linked positive trap entry before converting it to the
// privileged negative physical alias required by rT and rTT.
static inline int
mmix_trap_vector_make(uint64 entry, uint64 text_end, uint64 *vector)
{
  if (vector == 0 || text_end <= KERNEL_LOAD || text_end > KERNEL_LIMIT ||
      entry < KERNEL_LOAD || entry >= text_end ||
      (entry & (MMIX_TRAP_VECTOR_ALIGN - 1)) != 0 ||
      (entry & MMIX_PHYSICAL_ALIAS_BIT) != 0)
    return -1;

  *vector = mmix_phys_alias(entry);
  return 0;
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

static inline uint64
mmix_sp_read(void)
{
  uint64 value;

  asm volatile("OR %0, r254, 0" : "=r"(value));
  return value;
}

#define MMIX_DEFINE_SR_READ(function, reg)                                   \
  static inline uint64 function(void)                                        \
  {                                                                          \
    uint64 value;                                                            \
                                                                             \
    asm volatile("GET %0, " #reg : "=r"(value));                           \
    return value;                                                            \
  }

#define MMIX_DEFINE_SR_WRITE(function, reg)                                  \
  static inline void function(uint64 value)                                  \
  {                                                                          \
    asm volatile("PUT " #reg ", %0" : : "r"(value) : "memory");          \
  }

MMIX_DEFINE_SR_READ(mmix_ra_read, rA)
// Clang requires explicit floating-environment modeling for PUT rA and permits
// it only in module-level assembly, so the write accessor lives there.
void mmix_ra_write(uint64 value);
// GET rQ begins the architectural GET/PUT request handoff. A caller that
// services requests must finish that handoff with mmix_rq_write().
MMIX_DEFINE_SR_READ(mmix_rq_read, rQ)
MMIX_DEFINE_SR_WRITE(mmix_rq_write, rQ)
MMIX_DEFINE_SR_READ(mmix_rt_read, rT)
MMIX_DEFINE_SR_WRITE(mmix_rt_write, rT)
MMIX_DEFINE_SR_READ(mmix_rtt_read, rTT)
MMIX_DEFINE_SR_WRITE(mmix_rtt_write, rTT)

MMIX_DEFINE_SR_READ(mmix_rb_read, rB)
MMIX_DEFINE_SR_WRITE(mmix_rb_write, rB)
MMIX_DEFINE_SR_READ(mmix_rw_read, rW)
MMIX_DEFINE_SR_WRITE(mmix_rw_write, rW)
MMIX_DEFINE_SR_READ(mmix_rx_read, rX)
MMIX_DEFINE_SR_WRITE(mmix_rx_write, rX)
MMIX_DEFINE_SR_READ(mmix_ry_read, rY)
MMIX_DEFINE_SR_WRITE(mmix_ry_write, rY)
MMIX_DEFINE_SR_READ(mmix_rz_read, rZ)
MMIX_DEFINE_SR_WRITE(mmix_rz_write, rZ)

MMIX_DEFINE_SR_READ(mmix_rbb_read, rBB)
MMIX_DEFINE_SR_WRITE(mmix_rbb_write, rBB)
MMIX_DEFINE_SR_READ(mmix_rww_read, rWW)
MMIX_DEFINE_SR_WRITE(mmix_rww_write, rWW)
MMIX_DEFINE_SR_READ(mmix_rxx_read, rXX)
MMIX_DEFINE_SR_WRITE(mmix_rxx_write, rXX)
MMIX_DEFINE_SR_READ(mmix_ryy_read, rYY)
MMIX_DEFINE_SR_WRITE(mmix_ryy_write, rYY)
MMIX_DEFINE_SR_READ(mmix_rzz_read, rZZ)
MMIX_DEFINE_SR_WRITE(mmix_rzz_write, rZZ)

#undef MMIX_DEFINE_SR_WRITE
#undef MMIX_DEFINE_SR_READ

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

static inline void
mmix_rk_write(uint64 value)
{
  asm volatile("PUT rK, %0" : : "r"(value) : "memory");
}

extern uint64 mmix_trap_rk_shadow;
void mmix_intr_mask_write(uint64 mask);

static inline uint64
mmix_rq_program(uint64 value)
{
  return value & MMIX_RQ_PROGRAM_MASK;
}

static inline uint64
mmix_kernel_mask(uint64 device_mask)
{
  return MMIX_KERNEL_PROGRAM_MASK | (device_mask & MMIX_KERNEL_INTC_MASK);
}

static inline uint64
mmix_rq_deliverable(uint64 requests, uint64 mask)
{
  return requests & mask;
}

static inline int
mmix_rq_intc_pending(uint64 requests, uint64 mask)
{
  return (mmix_rq_deliverable(requests, mask) & MMIX_RQ_INTC) != 0;
}

static inline uint64
mmix_ra_disable_trips(uint64 value)
{
  return value & ~MMIX_RA_TRIP_ENABLE_MASK;
}

static inline int
mmix_intr_get(void)
{
  return (mmix_rk_read() & MMIX_KERNEL_INTC_MASK) != 0;
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
_Static_assert(MMIX_KERNEL_PROGRAM_MASK == 0x000000e400000000,
               "kernel program mask must match the trap ABI");
_Static_assert(MMIX_KERNEL_TRAP_MASK == 0x000000e400000100,
               "kernel trap mask must match the platform ABI");
_Static_assert((MMIX_KERNEL_FORCED_TRAP_INSN & MMIX_RQ_PROGRAM_MASK) == 0,
               "kernel forced trap must not resemble a program cause");
_Static_assert((MMIX_RQ_PROGRAM_MASK & MMIX_RQ_INTC) == 0,
               "program and controller requests must not overlap");
_Static_assert((KERNEL_LOAD & (MMIX_TRAP_VECTOR_ALIGN - 1)) == 0 &&
                 KERNEL_LIMIT < MMIX_PHYSICAL_ALIAS_BIT,
               "kernel text must admit a negative trap alias");

#define MMIX_ASSERT_TRAP_GLOBAL(reg)                                         \
  _Static_assert(                                                            \
    __builtin_offsetof(struct mmix_trap_state,                               \
                       globals[(reg) - MMIX_TRAP_GLOBAL_FIRST]) ==           \
      MMIX_TRAP_GLOBAL_OFFSET(reg),                                          \
    "MMIX trap global offset mismatch")

MMIX_ASSERT_TRAP_GLOBAL(231);
MMIX_ASSERT_TRAP_GLOBAL(232);
MMIX_ASSERT_TRAP_GLOBAL(233);
MMIX_ASSERT_TRAP_GLOBAL(234);
MMIX_ASSERT_TRAP_GLOBAL(235);
MMIX_ASSERT_TRAP_GLOBAL(236);
MMIX_ASSERT_TRAP_GLOBAL(237);
MMIX_ASSERT_TRAP_GLOBAL(238);
MMIX_ASSERT_TRAP_GLOBAL(239);
MMIX_ASSERT_TRAP_GLOBAL(240);
MMIX_ASSERT_TRAP_GLOBAL(241);
MMIX_ASSERT_TRAP_GLOBAL(242);
MMIX_ASSERT_TRAP_GLOBAL(243);
MMIX_ASSERT_TRAP_GLOBAL(244);
MMIX_ASSERT_TRAP_GLOBAL(245);
MMIX_ASSERT_TRAP_GLOBAL(246);
MMIX_ASSERT_TRAP_GLOBAL(247);
MMIX_ASSERT_TRAP_GLOBAL(248);
MMIX_ASSERT_TRAP_GLOBAL(249);
MMIX_ASSERT_TRAP_GLOBAL(250);
MMIX_ASSERT_TRAP_GLOBAL(251);
MMIX_ASSERT_TRAP_GLOBAL(252);
MMIX_ASSERT_TRAP_GLOBAL(253);
MMIX_ASSERT_TRAP_GLOBAL(254);

#undef MMIX_ASSERT_TRAP_GLOBAL

#define MMIX_ASSERT_TRAP_OFFSET(member, offset)                              \
  _Static_assert(__builtin_offsetof(struct mmix_trap_state, member) ==       \
                   (offset),                                                 \
                 "MMIX trap-state offset mismatch")

MMIX_ASSERT_TRAP_OFFSET(rj, MMIX_TRAP_RJ_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rbb, MMIX_TRAP_RBB_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rww, MMIX_TRAP_RWW_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rxx, MMIX_TRAP_RXX_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(ryy, MMIX_TRAP_RYY_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rzz, MMIX_TRAP_RZZ_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rq, MMIX_TRAP_RQ_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(restore_rk, MMIX_TRAP_RESTORE_RK_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rg, MMIX_TRAP_RG_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rl, MMIX_TRAP_RL_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(ro, MMIX_TRAP_RO_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rs, MMIX_TRAP_RS_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(ra, MMIX_TRAP_RA_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rd, MMIX_TRAP_RD_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(re, MMIX_TRAP_RE_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rh, MMIX_TRAP_RH_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rm, MMIX_TRAP_RM_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rr, MMIX_TRAP_RR_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rp, MMIX_TRAP_RP_OFFSET);
MMIX_ASSERT_TRAP_OFFSET(rf, MMIX_TRAP_RF_OFFSET);

#undef MMIX_ASSERT_TRAP_OFFSET

_Static_assert(sizeof(struct mmix_trap_state) == MMIX_TRAP_STATE_SIZE,
               "MMIX trap-state size mismatch");
_Static_assert(MMIX_TRAP_STACK_RESERVE >= MMIX_TRAP_STATE_SIZE,
               "MMIX trap stack reserve cannot hold the saved state");
_Static_assert(__alignof__(struct mmix_trap_state) == MMIX_TRAP_STATE_ALIGN,
               "MMIX trap-state alignment mismatch");
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
