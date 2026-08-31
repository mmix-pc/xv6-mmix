#ifndef XV6_PROC_H
#define XV6_PROC_H

#include "mmix.h"

// Assembly-visible layout of the fixed kernel-owned state surrounding a
// process's variable-length MMIX SAVE record.
#define MMIX_PROC_TRAPFRAME_USER_STATE_OFFSET   0
#define MMIX_PROC_TRAPFRAME_KERNEL_STATE_OFFSET 8
#define MMIX_PROC_TRAPFRAME_USER_RV_OFFSET      16
#define MMIX_PROC_TRAPFRAME_USER_RK_OFFSET      24
#define MMIX_PROC_TRAPFRAME_USER_RC_OFFSET      32
#define MMIX_PROC_TRAPFRAME_USER_RI_OFFSET      40
#define MMIX_PROC_TRAPFRAME_USER_RU_OFFSET      48
#define MMIX_PROC_TRAPFRAME_USER_RF_OFFSET      56
#define MMIX_PROC_TRAPFRAME_RBB_OFFSET          64
#define MMIX_PROC_TRAPFRAME_RWW_OFFSET          72
#define MMIX_PROC_TRAPFRAME_RXX_OFFSET          80
#define MMIX_PROC_TRAPFRAME_RYY_OFFSET          88
#define MMIX_PROC_TRAPFRAME_RZZ_OFFSET          96
#define MMIX_PROC_TRAPFRAME_RQ_OFFSET           104
#define MMIX_PROC_TRAPFRAME_FLAGS_OFFSET        112
#define MMIX_PROC_TRAPFRAME_RESERVED_OFFSET     120
#define MMIX_PROC_TRAPFRAME_TRANSLATION_R253_OFFSET 128
#define MMIX_PROC_TRAPFRAME_TRANSLATION_R254_OFFSET 136
#define MMIX_PROC_TRAPFRAME_SIZE                144
#define MMIX_PROC_TRAPFRAME_ALIGN               8

#define MMIX_PROC_USER_RK (MMIX_RQ_PROGRAM_MASK | MMIX_KERNEL_INTERRUPT_MASK)

// A CPU-local scratch pointer is published only while a process owns the live
// user continuation. ENTERING closes recursive reuse before SAVE completes.
#define MMIX_PROC_TRAPFRAME_READY    0
#define MMIX_PROC_TRAPFRAME_ACTIVE   1
#define MMIX_PROC_TRAPFRAME_ENTERING 2

#if !defined(__ASSEMBLER__)

#include "cpu.h"

// Fixed kernel-owned state surrounding the variable-length MMIX SAVE record.
// This page is never mapped into the user address space.
struct trapframe {
  uint64 user_state;
  uint64 kernel_state;
  uint64 user_rv;
  uint64 user_rk;
  uint64 user_rc;
  uint64 user_ri;
  uint64 user_ru;
  uint64 user_rf;
  uint64 rbb;
  uint64 rww;
  uint64 rxx;
  uint64 ryy;
  uint64 rzz;
  uint64 rq;
  uint64 flags;
  uint64 reserved;
  // Scratch used before SAVE has made the user registers process-owned.
  uint64 translation_r253;
  uint64 translation_r254;
};

#define MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(field, offset)                       \
  _Static_assert(__builtin_offsetof(struct trapframe, field) == (offset),      \
                 "MMIX user trapframe offset mismatch")

MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(
  user_state, MMIX_PROC_TRAPFRAME_USER_STATE_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(
  kernel_state, MMIX_PROC_TRAPFRAME_KERNEL_STATE_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(user_rv,
                                  MMIX_PROC_TRAPFRAME_USER_RV_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(user_rk,
                                  MMIX_PROC_TRAPFRAME_USER_RK_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(user_rc,
                                  MMIX_PROC_TRAPFRAME_USER_RC_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(user_ri,
                                  MMIX_PROC_TRAPFRAME_USER_RI_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(user_ru,
                                  MMIX_PROC_TRAPFRAME_USER_RU_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(user_rf,
                                  MMIX_PROC_TRAPFRAME_USER_RF_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(rbb, MMIX_PROC_TRAPFRAME_RBB_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(rww, MMIX_PROC_TRAPFRAME_RWW_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(rxx, MMIX_PROC_TRAPFRAME_RXX_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(ryy, MMIX_PROC_TRAPFRAME_RYY_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(rzz, MMIX_PROC_TRAPFRAME_RZZ_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(rq, MMIX_PROC_TRAPFRAME_RQ_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(flags, MMIX_PROC_TRAPFRAME_FLAGS_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(
  reserved, MMIX_PROC_TRAPFRAME_RESERVED_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(
  translation_r253, MMIX_PROC_TRAPFRAME_TRANSLATION_R253_OFFSET);
MMIX_ASSERT_PROC_TRAPFRAME_OFFSET(
  translation_r254, MMIX_PROC_TRAPFRAME_TRANSLATION_R254_OFFSET);

#undef MMIX_ASSERT_PROC_TRAPFRAME_OFFSET

_Static_assert(sizeof(struct trapframe) == MMIX_PROC_TRAPFRAME_SIZE,
               "MMIX user trapframe size mismatch");
_Static_assert(__alignof__(struct trapframe) == MMIX_PROC_TRAPFRAME_ALIGN,
               "MMIX user trapframe alignment mismatch");
_Static_assert(MMIX_PROC_TRAPFRAME_TRANSLATION_R254_OFFSET + sizeof(uint64) ==
                 MMIX_PROC_TRAPFRAME_SIZE,
               "user trapframe offsets must cover its fixed header");

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state; // Process state
  void *chan;           // If non-zero, sleeping on chan
  int killed;           // If non-zero, have been killed
  int xstate;           // Exit status to be returned to parent's wait
  int pid;              // Process ID
  int vm_owner_cpu;     // CPU owning this address space, or -1.
  int resume_cpu;       // Required CPU for a live CPU-owned continuation.
  uint64 vm_generation; // Completed page-table mutation generation.

  // Residency changes are serialized by p->lock. Each CPU's local generation
  // is also suitable for target-local atomic shootdown completion.
  uint64 vm_resident_cpus; // CPUs that may retain this slot's ASN.
  uint64 vm_cpu_generation[NCPU];

  // wait_lock must be held when using this:
  struct proc *parent; // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  uint64 lazy_start;           // First address published by lazy sbrk
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // Kernel-owned MMIX user transition state
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};

// MMIX address-space ownership shared by the process, VM, and trap modules.
enum vm_invalidation_class {
  VM_INVALIDATE_INSTRUCTION = 1 << 0,
  VM_INVALIDATE_DATA = 1 << 1,
  VM_INVALIDATE_ALL = VM_INVALIDATE_INSTRUCTION | VM_INVALIDATE_DATA,
};

void proc_vm_mutated(struct proc *p);
void proc_vm_prepare_user(struct proc *p);
void proc_vm_begin_mutation(struct proc *p);
void proc_vm_commit_mutation(struct proc *p, uint64 va, uint64 classes);
int proc_vm_ipi_work(uint64 classes, uint64 generation);

#endif // !__ASSEMBLER__

#endif // XV6_PROC_H
