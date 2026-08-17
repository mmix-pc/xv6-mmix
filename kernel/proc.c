#include "types.h"
#include "param.h"
#include "mmix.h"
#include "spinlock.h"
#include "proc.h"
#include "kcontext.h"
#include "defs.h"

struct proc proc[NPROC];

static int nextpid = 1;
static struct spinlock pid_lock;

static int
allocpid(void)
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid++;
  release(&pid_lock);
  return pid;
}

static int
proc_user_state_empty(struct proc *p)
{
  if (p->pagetable != 0 || p->trapframe != 0 || p->cwd != 0 || p->sz != 0 ||
      p->parent != 0)
    return 0;
  for (int fd = 0; fd < NOFILE; fd++)
    if (p->ofile[fd] != 0)
      return 0;
  return 1;
}

static int
proc_slot_clean(struct proc *p)
{
  return p->chan == 0 && p->killed == 0 && p->xstate == 0 && p->pid == 0 &&
         p->name[0] == 0 && p->context.state == 0 &&
         proc_user_state_empty(p);
}

static void
proc_clear(struct proc *p, int clear_context)
{
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->pid = 0;
  p->parent = 0;
  p->sz = 0;
  p->pagetable = 0;
  p->trapframe = 0;
  for (int fd = 0; fd < NOFILE; fd++)
    p->ofile[fd] = 0;
  p->cwd = 0;
  p->name[0] = 0;
  if (clear_context)
    memset(&p->context, 0, sizeof(p->context));
}

// Allocate a process-table slot and its initial kernel context. Return with the
// slot lock held so the caller can finish initialization before publication.
struct proc *
proc_alloc(void (*entry)(void))
{
  struct proc *p;

  if (entry == 0)
    panic("proc entry");
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      if (!proc_slot_clean(p))
        panic("proc dirty slot");
      proc_clear(p, 1);
      p->pid = allocpid();
      p->state = USED;
      mmix_kcontext_prepare(&p->context,
                            MMIX_PROCESS_CONTEXT_SLOT((uint)(p - proc)), entry);
      return p;
    }
    release(&p->lock);
  }
  return 0;
}

// Publish a fully initialized process to the scheduler.
void
proc_start(struct proc *p)
{
  if (p == 0 || !holding(&p->lock) || p->state != USED ||
      p->context.state == 0)
    panic("proc start");
  p->state = RUNNABLE;
  release(&p->lock);
}

// Release a process slot after its caller has freed all attached resources.
void
proc_release(struct proc *p)
{
  if (p == 0 || !holding(&p->lock) ||
      (p->state != USED && p->state != ZOMBIE))
    panic("proc release");
  if (p->pagetable != 0 || p->trapframe != 0 || p->cwd != 0 || p->sz != 0)
    panic("proc release resource");
  for (int fd = 0; fd < NOFILE; fd++)
    if (p->ofile[fd] != 0)
      panic("proc release file");
  proc_clear(p, 1);
  p->state = UNUSED;
}

// Initialize the kernel scheduling state of every process-table slot. User
// address spaces and user-entry state are initialized by later lifecycle code.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
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

        if (!holding(&p->lock) || c->noff != 1 || intr_get() ||
            p->state == RUNNING)
          panic("scheduler state");
        if (p->state == UNUSED) {
          if (p->chan != 0 || !proc_user_state_empty(p))
            panic("scheduler release");
          memset(&p->context, 0, sizeof(p->context));
        }
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

  if (p == 0)
    panic("sched proc");
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

  if (p == 0)
    panic("yield proc");
  acquire(&p->lock);
  if (p->state != RUNNING)
    panic("yield state");
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// Atomically release a condition lock and sleep on chan. The process keeps
// exclusive ownership of its software and register stacks while asleep.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0 || chan == 0 || lk == 0 || lk == &p->lock || !holding(lk))
    panic("sleep lock");
  acquire(&p->lock);
  release(lk);

  p->chan = chan;
  p->state = SLEEPING;
  sched();

  if (!holding(&p->lock) || p->state != RUNNING || p->chan != chan)
    panic("sleep wake");
  p->chan = 0;
  release(&p->lock);
  acquire(lk);
}

// Wake every process sleeping on chan. The caller's condition lock protects
// the predicate; p->lock protects the SLEEPING-to-RUNNABLE transition.
void
wakeup(void *chan)
{
  struct proc *p;
  struct proc *self = myproc();

  if (chan == 0 || self == 0)
    panic("wakeup chan");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p != self) {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
        p->state = RUNNABLE;
      release(&p->lock);
    }
  }
}
