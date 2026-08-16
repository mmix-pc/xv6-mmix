#ifndef XV6_MMIX_CPU_H
#define XV6_MMIX_CPU_H

#include "param.h"
#include "types.h"

struct proc;

// SAVE owns the complete suspended MMIX register state in its register-stack
// record. UNSAVE consumes this address when the context is resumed.
struct context {
  uint64 state;
};

// Per-CPU scheduler state.
struct cpu {
  struct proc *proc;      // The process running on this CPU, or null.
  struct context context; // swtch() here to enter the scheduler.
  int noff;               // Depth of push_off() nesting.
  int intena;             // Whether interrupts were enabled before push_off().
};

extern struct cpu cpus[NCPU];

int cpuid(void);
struct cpu *mycpu(void);
int intr_get(void);
void intr_off(void);
void intr_on(void);
void cpu_idle(void);

#endif
