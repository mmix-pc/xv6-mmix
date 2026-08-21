#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmix.h"
#include "spinlock.h"
#include "proc.h"
#include "kcontext.h"
#include "kalloc.h"
#include "vm.h"
#include "defs.h"

enum {
  MMIX_USER_INITIAL_GLOBAL_COUNT = 256 - MMIX_ABI_GLOBAL_FIRST,
};

struct mmix_initial_user_context {
  uint64 outer_hole;
  uint64 local_hole;
  uint64 globals[MMIX_USER_INITIAL_GLOBAL_COUNT];
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

_Static_assert(__builtin_offsetof(struct mmix_initial_user_context, rg_ra) ==
                 MMIX_CONTEXT_INITIAL_STATE_OFFSET,
               "MMIX initial user state offset mismatch");
_Static_assert(sizeof(struct mmix_initial_user_context) ==
                 MMIX_CONTEXT_INITIAL_SIZE,
               "MMIX initial user state size mismatch");
_Static_assert(NPROC == MMIX_USER_ASN_LAST - MMIX_USER_ASN_FIRST + 1,
               "process slots must have one MMIX user ASN each");

struct proc proc[NPROC];

static int nextpid = 1;
static struct spinlock pid_lock;
// Protects parent linkage and prevents a child exit from racing with wait.
static struct spinlock wait_lock;

static uint
proc_index(struct proc *p)
{
  if (p < proc || p >= &proc[NPROC])
    panic("proc user slot");
  return (uint)(p - proc);
}

static int
user_mapping_has(pagetable_t pagetable, uint64 va, uint64 permissions)
{
  pte_t *leaf = walk(pagetable, va, 0);

  return leaf != 0 &&
         *leaf == mmix_pte_make(mmix_pte_pa(*leaf),
                                MMIX_RV_N(pagetable->rv),
                                mmix_pte_permissions(*leaf)) &&
         kalloc_page_is_managed((void *)mmix_pte_pa(*leaf)) &&
         (mmix_pte_permissions(*leaf) & permissions) == permissions;
}

static int
user_stacks_valid(pagetable_t pagetable)
{
  for (uint64 va = MMIX_USER_STACK_BASE; va < MMIX_USER_STACK_TOP;
       va += PGSIZE)
    if (!user_mapping_has(pagetable, va, PTE_R | PTE_W))
      return 0;
  for (uint64 va = MMIX_USER_REGISTER_STACK_BASE;
       va < MMIX_USER_REGISTER_STACK_TOP; va += PGSIZE)
    if (!user_mapping_has(pagetable, va, PTE_R | PTE_W))
      return 0;
  return walk(pagetable, MMIX_USER_STACK_GUARD_BASE, 0) == 0 &&
         walk(pagetable, MMIX_USER_REGISTER_GUARD_BASE, 0) == 0 &&
         walk(pagetable, MMIX_USER_REGISTER_GUARD_TOP - PGSIZE, 0) == 0;
}

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
static struct proc *
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
static void
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
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

pagetable_t
proc_pagetable(struct proc *p)
{
  uint index;

  if (p == 0 || !holding(&p->lock) || p->state != USED ||
      p->pagetable != 0)
    panic("proc pagetable");
  index = proc_index(p);
  return uvmcreate(MMIX_USER_ASN_FIRST + index);
}

void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  if (pagetable == 0)
    panic("proc free pagetable");
  uvmfree(pagetable, sz);
}

static void
proc_user_free(struct proc *p)
{
  if (p == 0 || !holding(&p->lock) ||
      (p->state != USED && p->state != ZOMBIE))
    panic("proc user free");
  if (p->pagetable != 0) {
    proc_freepagetable(p->pagetable, p->sz);
    p->pagetable = 0;
  }
  p->sz = 0;
  if (p->trapframe != 0) {
    kfree(p->trapframe);
    p->trapframe = 0;
  }
  proc_release(p);
}

static int
proc_user_resources(struct proc *p, int allocate_stacks)
{
  if ((p->trapframe = kalloc()) == 0)
    return -1;
  memset(p->trapframe, 0, PGSIZE);

  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
    return -1;
  p->sz = MMIX_USER_IMAGE_BASE;
  p->trapframe->user_rv = p->pagetable->rv;
  p->trapframe->user_rk = MMIX_PROC_USER_RK;

  if (allocate_stacks && uvmallocstacks(p->pagetable) < 0)
    return -1;
  return 0;
}

static struct proc *
proc_user_alloc_internal(void (*kernel_entry)(void), int allocate_stacks)
{
  struct proc *p = proc_alloc(kernel_entry);

  if (p == 0)
    return 0;
  if (proc_user_resources(p, allocate_stacks) < 0) {
    proc_user_free(p);
    release(&p->lock);
    return 0;
  }
  return p;
}

// Allocate a complete, unpublished user process. The returned process is in
// USED state with its lock held; its image may be populated before proc_start.
static __attribute__((used)) struct proc *
proc_user_alloc(void (*kernel_entry)(void))
{
  return proc_user_alloc_internal(kernel_entry, 1);
}

// User entry will consume this once the trap return path is connected.
static __attribute__((used)) int
proc_user_init(struct proc *p, uint64 entry)
{
  struct mmix_initial_user_context initial;

  if (p == 0 || !holding(&p->lock) || p->state != USED ||
      p->pagetable == 0 || p->trapframe == 0 ||
      p->trapframe->user_state != 0 || entry < MMIX_USER_IMAGE_BASE ||
      entry >= p->sz || !user_mapping_has(p->pagetable, entry, PTE_X) ||
      !user_stacks_valid(p->pagetable))
    return -1;

  memset(&initial, 0, sizeof(initial));
  initial.globals[MMIX_ABI_FP - MMIX_ABI_GLOBAL_FIRST] =
    MMIX_USER_STACK_TOP;
  initial.globals[MMIX_ABI_SP - MMIX_ABI_GLOBAL_FIRST] =
    MMIX_USER_STACK_TOP;
  initial.rj = entry;
  initial.rg_ra = (uint64)MMIX_ABI_GLOBAL_FIRST << 56;
  if (copyout(p->pagetable, MMIX_USER_REGISTER_STACK_BASE, (char *)&initial,
              sizeof(initial)) < 0)
    return -1;

  memset(p->trapframe, 0, sizeof(*p->trapframe));
  p->trapframe->user_state =
    MMIX_USER_REGISTER_STACK_BASE + MMIX_CONTEXT_INITIAL_STATE_OFFSET;
  p->trapframe->user_rv = p->pagetable->rv;
  p->trapframe->user_rk = MMIX_PROC_USER_RK;
  p->trapframe->rww = entry;
  p->trapframe->rxx = MMIX_DYNAMIC_TRAP_RESUME_NEXT;
  return 0;
}

static int
proc_user_set_result(struct proc *p, uint64 value)
{
  uint64 state;
  uint64 address;

  if (p == 0 || p->trapframe == 0)
    return -1;
  state = p->trapframe->user_state;
  if ((state & (sizeof(uint64) - 1)) != 0 ||
      state < MMIX_USER_REGISTER_STACK_BASE -
                MMIX_SAVED_GLOBAL_OFFSET(MMIX_ABI_GLOBAL_FIRST) ||
      state >= MMIX_USER_REGISTER_STACK_TOP)
    return -1;
  address = state + MMIX_SAVED_GLOBAL_OFFSET(MMIX_ABI_GLOBAL_FIRST);
  return copyout(p->pagetable, address, (char *)&value, sizeof(value));
}

// Clone only architecture-neutral process state. File descriptors, cwd,
// parent linkage, and scheduler publication remain the caller's responsibility.
// The returned child is in USED state with its lock held.
static __attribute__((used)) struct proc *
proc_user_clone(struct proc *parent, void (*kernel_entry)(void))
{
  struct proc *child;

  if (parent == 0 || parent->pagetable == 0 || parent->trapframe == 0 ||
      parent->sz < MMIX_USER_IMAGE_BASE ||
      parent->trapframe->user_state == 0 ||
      parent->trapframe->user_rv != parent->pagetable->rv ||
      parent->trapframe->user_rk != MMIX_PROC_USER_RK)
    return 0;

  child = proc_user_alloc_internal(kernel_entry, 0);
  if (child == 0)
    return 0;
  if (uvmcopy(parent->pagetable, child->pagetable, parent->sz) < 0)
    goto fail;

  child->sz = parent->sz;
  *child->trapframe = *parent->trapframe;
  child->trapframe->kernel_state = 0;
  child->trapframe->user_rv = child->pagetable->rv;
  child->trapframe->user_rk = MMIX_PROC_USER_RK;
  child->trapframe->rq = 0;
  child->trapframe->flags = 0;
  child->trapframe->reserved = 0;
  if (proc_user_set_result(child, 0) < 0)
    goto fail;
  safestrcpy(child->name, parent->name, sizeof(child->name));
  return child;

fail:
  proc_user_free(child);
  release(&child->lock);
  return 0;
}

static void
proc_user_entry(void)
{
  struct proc *p = myproc();

  if (p == 0 || !holding(&p->lock) || p->state != RUNNING)
    panic("user process entry");
  release(&p->lock);
  for (;;) {
    usertrapret();
    usertrap();
  }
}

int
kfork(void)
{
  struct proc *parent = myproc();
  struct proc *child;
  int pid;

  if (parent == 0 || parent->cwd != 0)
    return -1;
  // FIXME: duplicate descriptor and cwd references when filesystem-backed
  // user processes are enabled.
  for (int fd = 0; fd < NOFILE; fd++)
    if (parent->ofile[fd] != 0)
      return -1;

  child = proc_user_clone(parent, proc_user_entry);
  if (child == 0)
    return -1;
  pid = child->pid;
  release(&child->lock);

  acquire(&wait_lock);
  child->parent = parent;
  release(&wait_lock);

  acquire(&child->lock);
  proc_start(child);
  return pid;
}

static int
proc_user_grow(struct proc *p, int n)
{
  uint64 oldsz;
  uint64 newsz;

  if (p == 0 || p->pagetable == 0 || p->sz < MMIX_USER_IMAGE_BASE)
    return -1;
  oldsz = p->sz;
  if (n > 0) {
    if ((uint64)n > MMIX_USER_HEAP_LIMIT - oldsz)
      return -1;
    newsz = oldsz + (uint64)n;
    if (uvmalloc(p->pagetable, oldsz, newsz, PTE_W) == 0)
      return -1;
  } else if (n < 0) {
    uint64 amount = (uint64)(-(long)n);

    if (amount > oldsz - MMIX_USER_IMAGE_BASE)
      return -1;
    newsz = oldsz - amount;
    if (uvmdealloc(p->pagetable, oldsz, newsz) != newsz)
      return -1;
  } else {
    return 0;
  }
  p->sz = newsz;
  return 0;
}

int
growproc(int n)
{
  return proc_user_grow(myproc(), n);
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
    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      // No process or process stack is owned here. Enable timer delivery before
      // acquire() closes the state-transition and context-switch window.
      intr_on();
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

static void
reparent(struct proc *p)
{
  struct proc *new_parent = p->parent;

  for (struct proc *child = proc; child < &proc[NPROC]; child++)
    if (child->parent == p)
      child->parent = new_parent;
  if (new_parent != 0)
    wakeup(new_parent);
}

// Exit the current process without returning through its user continuation.
// Its address space remains owned by the ZOMBIE until its parent reaps it.
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("exit proc");
  // FIXME: replace these invariants with descriptor and cwd teardown before
  // filesystem-backed process creation is enabled.
  if (p->cwd != 0)
    panic("exit cwd");
  for (int fd = 0; fd < NOFILE; fd++)
    if (p->ofile[fd] != 0)
      panic("exit file");

  acquire(&wait_lock);
  reparent(p);
  if (p->parent != 0)
    wakeup(p->parent);
  acquire(&p->lock);
  if (p->state != RUNNING)
    panic("exit state");
  p->xstate = status;
  p->state = ZOMBIE;
  release(&wait_lock);
  sched();
  panic("zombie exit");
}

int
kwait(uint64 address)
{
  struct proc *p = myproc();

  acquire(&wait_lock);
  for (;;) {
    int have_children = 0;

    for (struct proc *child = proc; child < &proc[NPROC]; child++) {
      if (child->parent != p)
        continue;
      acquire(&child->lock);
      have_children = 1;
      if (child->state == ZOMBIE) {
        int pid = child->pid;

        if (address != 0 &&
            copyout(p->pagetable, address, (char *)&child->xstate,
                    sizeof(child->xstate)) < 0) {
          release(&child->lock);
          release(&wait_lock);
          return -1;
        }
        proc_user_free(child);
        release(&child->lock);
        release(&wait_lock);
        return pid;
      }
      release(&child->lock);
    }
    if (!have_children || killed(p)) {
      release(&wait_lock);
      return -1;
    }
    sleep(p, &wait_lock);
  }
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

  if (chan == 0)
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

int
kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED) {
      p->killed = 1;
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int value;

  acquire(&p->lock);
  value = p->killed;
  release(&p->lock);
  return value;
}

int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();

  if (user_dst)
    return p == 0 ? -1 : copyout(p->pagetable, dst, src, len);
  memmove((void *)dst, src, len);
  return 0;
}

int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();

  if (user_src)
    return p == 0 ? -1 : copyin(p->pagetable, dst, src, len);
  memmove(dst, (void *)src, len);
  return 0;
}
