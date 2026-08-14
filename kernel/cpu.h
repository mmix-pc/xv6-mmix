#ifndef XV6_MMIX_CPU_H
#define XV6_MMIX_CPU_H

#include "param.h"

struct proc;

// Per-CPU state needed before the scheduler is ported.
struct cpu {
  struct proc *proc; // The process running on this CPU, or null.
  int noff;          // Depth of push_off() nesting.
  int intena;        // Whether interrupts were enabled before push_off().
};

extern struct cpu cpus[NCPU];

int cpuid(void);
struct cpu *mycpu(void);
int intr_get(void);
void intr_off(void);
void intr_on(void);

#endif
