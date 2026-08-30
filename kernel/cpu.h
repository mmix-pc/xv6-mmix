#ifndef XV6_MMIX_CPU_H
#define XV6_MMIX_CPU_H

// Assembly-visible struct cpu layout.
#define MMIX_CPU_PROC_OFFSET           0
#define MMIX_CPU_CONTEXT_OFFSET        8
#define MMIX_CPU_NOFF_OFFSET           16
#define MMIX_CPU_INTENA_OFFSET         20
#define MMIX_CPU_TRAP_ACTIVE_OFFSET    24
#define MMIX_CPU_TRAP_RK_SHADOW_OFFSET 32
#define MMIX_CPU_INTERRUPT_ENTRIES_OFFSET 40
#define MMIX_CPU_INTERRUPT_RETURNS_OFFSET 48
#define MMIX_CPU_SIZE                     56

#if !defined(__ASSEMBLER__)

#include "param.h"
#include "types.h"

struct proc;

// SAVE owns the complete suspended MMIX register state in its register-stack
// record. UNSAVE consumes this address when the context is resumed.
struct context {
  uint64 state;
};

// Mutable dynamic-trap state owned by one CPU.
struct cpu_trap_state {
  volatile uint64 active;
  uint64 rk_shadow;
  uint64 interrupt_entries;
  uint64 interrupt_returns;
};

// Per-CPU scheduler state.
struct cpu {
  struct proc *proc;             // The running process, or null.
  struct context context;        // swtch() here to enter the scheduler.
  int noff;                      // Depth of push_off() nesting.
  int intena;                    // Interrupt state before push_off().
  struct cpu_trap_state trap;    // CPU-owned dynamic-trap state.
};

#define MMIX_ASSERT_CPU_OFFSET(member, offset)                               \
  _Static_assert(__builtin_offsetof(struct cpu, member) == (offset),         \
                 "MMIX CPU offset mismatch")

MMIX_ASSERT_CPU_OFFSET(proc, MMIX_CPU_PROC_OFFSET);
MMIX_ASSERT_CPU_OFFSET(context, MMIX_CPU_CONTEXT_OFFSET);
MMIX_ASSERT_CPU_OFFSET(noff, MMIX_CPU_NOFF_OFFSET);
MMIX_ASSERT_CPU_OFFSET(intena, MMIX_CPU_INTENA_OFFSET);
_Static_assert(__builtin_offsetof(struct cpu, trap.active) ==
                 MMIX_CPU_TRAP_ACTIVE_OFFSET,
               "MMIX CPU trap-active offset mismatch");
_Static_assert(__builtin_offsetof(struct cpu, trap.rk_shadow) ==
                 MMIX_CPU_TRAP_RK_SHADOW_OFFSET,
               "MMIX CPU trap-mask offset mismatch");
_Static_assert(__builtin_offsetof(struct cpu, trap.interrupt_entries) ==
                 MMIX_CPU_INTERRUPT_ENTRIES_OFFSET,
               "MMIX CPU interrupt-entry offset mismatch");
_Static_assert(__builtin_offsetof(struct cpu, trap.interrupt_returns) ==
                 MMIX_CPU_INTERRUPT_RETURNS_OFFSET,
               "MMIX CPU interrupt-return offset mismatch");
_Static_assert(sizeof(struct cpu) == MMIX_CPU_SIZE,
               "MMIX CPU size mismatch");

#undef MMIX_ASSERT_CPU_OFFSET

extern struct cpu cpus[NCPU];

int cpuid(void);
struct cpu *mycpu(void);
int intr_get(void);
void intr_off(void);
void intr_on(void);
void cpu_idle(void);
void cpu_secondary_enter(void (*)(void)) __attribute__((noreturn));

#endif // !__ASSEMBLER__

#endif
