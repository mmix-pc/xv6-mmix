#include "types.h"
#include "param.h"
#include "mmix.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct proc proc[NPROC];

// Initialize the kernel scheduling state of every process-table slot. User
// address spaces and user-entry state are initialized by later lifecycle code.
void
procinit(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  struct proc *p;

  push_off();
  p = mycpu()->proc;
  pop_off();
  return p;
}

// Per-CPU process scheduler. A process switches out holding p->lock; this
// loop releases that lock only after the scheduler context is restored.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // Allow pending work to be delivered, then close the switch window before
    // inspecting process state or changing stack ownership.
    intr_on();
    intr_off();

    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0)
      cpu_idle();
  }
}

// Switch to the scheduler. The caller must hold only p->lock and must have
// already changed p->state away from RUNNING.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}
