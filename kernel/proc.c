#include "types.h"
#include "param.h"
#include "boot.h"
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

struct cpu cpus[NCPU];
struct proc proc[NPROC];

static int nextpid = 1;
static struct spinlock pid_lock;
// Protects parent linkage and prevents a child exit from racing with wait.
static struct spinlock wait_lock;
static struct proc *initproc;

enum {
  MMIX_VM_STATE_ASN_BITS = 10,
};

#define MMIX_VM_GENERATION_LIMIT (~(uint64)0 >> MMIX_VM_STATE_ASN_BITS)

_Static_assert(MMIX_RV_N_VALUE_MASK ==
                 ((1U << MMIX_VM_STATE_ASN_BITS) - 1),
               "VM state must preserve the complete MMIX ASN");

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
      p->lazy_start != 0 || p->parent != 0)
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
         p->vm_owner_cpu == -1 && p->resume_cpu == -1 &&
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
  p->vm_owner_cpu = -1;
  p->resume_cpu = -1;
  p->parent = 0;
  p->sz = 0;
  p->lazy_start = 0;
  p->pagetable = 0;
  p->trapframe = 0;
  for (int fd = 0; fd < NOFILE; fd++)
    p->ofile[fd] = 0;
  p->cwd = 0;
  p->name[0] = 0;
  if (clear_context)
    memset(&p->context, 0, sizeof(p->context));
}

static void
proc_vm_advance_locked(struct proc *p)
{
  if (p == 0 || !holding(&p->lock) || p->vm_owner_cpu < -1 ||
      p->vm_owner_cpu >= NCPU ||
      p->vm_generation == MMIX_VM_GENERATION_LIMIT)
    panic("vm generation");
  p->vm_generation++;
}

static uint64
proc_vm_state(struct proc *p)
{
  uint64 asn;

  if (p == 0 || p->pagetable == 0 || p->vm_generation == 0 ||
      p->vm_generation > MMIX_VM_GENERATION_LIMIT)
    panic("vm state");
  asn = MMIX_RV_N(p->pagetable->rv);
  if (asn < MMIX_USER_ASN_FIRST || asn > MMIX_USER_ASN_LAST)
    panic("vm asn");
  return (p->vm_generation << MMIX_VM_STATE_ASN_BITS) | asn;
}

static void
proc_vm_claim_locked(struct proc *p, struct cpu *c)
{
  int id = cpuid();

  if (p == 0 || c == 0 || !holding(&p->lock) || p->state != RUNNABLE ||
      p->vm_owner_cpu != -1 || c != &cpus[id] || c->proc != 0)
    panic("vm claim");
  if (p->pagetable == 0)
    return;
  (void)proc_vm_state(p);
  p->vm_owner_cpu = id;
}

static void
proc_vm_release_locked(struct proc *p, struct cpu *c)
{
  int id = cpuid();

  if (p == 0 || c == 0 || !holding(&p->lock) || p->state == RUNNING ||
      c != &cpus[id] || c->proc != p || c->trap.user_trapframe != 0 ||
      mmix_rv_read() != MMIX_KERNEL_RV)
    panic("vm release");
  if (p->pagetable == 0) {
    if (p->vm_owner_cpu != -1)
      panic("vm kernel owner");
    return;
  }
  if (p->vm_owner_cpu != id)
    panic("vm release owner");
  p->vm_owner_cpu = -1;
}

void
proc_vm_mutated(struct proc *p)
{
  struct cpu *c = mycpu();

  if (p == 0 || !holding(&p->lock) || p->state != RUNNING || c->proc != p ||
      p->vm_owner_cpu != cpuid() || p->pagetable == 0 ||
      mmix_rv_read() != MMIX_KERNEL_RV)
    panic("vm mutation");
  proc_vm_advance_locked(p);
}

// Establish a fresh local translation view before assembly loads the user rV.
void
proc_vm_prepare_user(struct proc *p)
{
  struct cpu *c = mycpu();
  uint64 state;

  if (p == 0 || c->proc != p || p->state != RUNNING ||
      p->vm_owner_cpu != cpuid() || holding(&p->lock) || intr_get() ||
      mmix_rv_read() != MMIX_KERNEL_RV)
    panic("vm prepare");
  state = proc_vm_state(p);
  if (c->user_translation == state)
    return;

  // P2.5 invalidates only this CPU; P2.6 adds acknowledged remote shootdown.
  mmix_rv_publish(MMIX_KERNEL_RV);
  if (mmix_rv_read() != MMIX_KERNEL_RV)
    panic("vm invalidate");
  c->user_translation = state;
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
      kcontext_prepare(&p->context,
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
      p->context.state == 0 || p->vm_owner_cpu != -1 || p->resume_cpu != -1)
    panic("proc start");
  // Publish all construction-time page-table work as one completed state.
  if (p->pagetable != 0)
    proc_vm_advance_locked(p);
  p->state = RUNNABLE;
  release(&p->lock);
}

// Release a process slot after its caller has freed all attached resources.
static void
proc_release(struct proc *p)
{
  if (p == 0 || !holding(&p->lock) ||
      (p->state != USED && p->state != ZOMBIE) || p->vm_owner_cpu != -1 ||
      p->resume_cpu != -1)
    panic("proc release");
  if (p->pagetable != 0 || p->trapframe != 0 || p->cwd != 0 || p->sz != 0 ||
      p->lazy_start != 0)
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
    p->vm_owner_cpu = -1;
    p->resume_cpu = -1;
  }
}

pagetable_t
proc_pagetable(struct proc *p)
{
  uint index;

  if (p == 0)
    return 0;
  // A replacement image uses the process slot's existing ASN but remains
  // inactive until proc_exec commits it.
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
      (p->state != USED && p->state != ZOMBIE) || p->vm_owner_cpu != -1)
    panic("proc user free");
  if (p->pagetable != 0) {
    proc_freepagetable(p->pagetable, p->sz);
    p->pagetable = 0;
  }
  p->sz = 0;
  p->lazy_start = 0;
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

static int
proc_user_context(pagetable_t pagetable, struct trapframe *trapframe,
                  uint64 entry, uint64 stack, uint64 argc, uint64 argv)
{
  struct mmix_initial_user_context initial;
  uint64 argbase = MMIX_USER_STACK_TOP - MMIX_EXEC_ARG_MAX;
  uint64 argv_size;

  if (pagetable == 0 || trapframe == 0 || entry < MMIX_USER_IMAGE_BASE ||
      !user_mapping_has(pagetable, entry, PTE_X) ||
      !user_stacks_valid(pagetable) || argc > MAXARG ||
      (stack & (sizeof(uint64) - 1)) != 0 || stack < argbase ||
      stack > MMIX_USER_STACK_TOP)
    return -1;
  argv_size = (argc + 1) * sizeof(uint64);
  if ((argc != 0 && argv == 0) ||
      (argv != 0 && ((argv & (sizeof(uint64) - 1)) != 0 || argv < stack ||
                     argv > MMIX_USER_STACK_TOP - argv_size)))
    return -1;

  memset(&initial, 0, sizeof(initial));
  initial.globals[0] = argc;
  initial.globals[1] = argv;
  initial.globals[MMIX_ABI_FP - MMIX_ABI_GLOBAL_FIRST] = stack;
  initial.globals[MMIX_ABI_SP - MMIX_ABI_GLOBAL_FIRST] = stack;
  initial.rj = entry;
  initial.rg_ra = (uint64)MMIX_ABI_GLOBAL_FIRST << 56;
  if (copyout(pagetable, MMIX_USER_REGISTER_STACK_BASE, (char *)&initial,
              sizeof(initial)) < 0)
    return -1;

  memset(trapframe, 0, sizeof(*trapframe));
  trapframe->user_state =
    MMIX_USER_REGISTER_STACK_BASE + MMIX_CONTEXT_INITIAL_STATE_OFFSET;
  trapframe->user_rv = pagetable->rv;
  trapframe->user_rk = MMIX_PROC_USER_RK;
  trapframe->rww = entry;
  trapframe->rxx = MMIX_DYNAMIC_TRAP_RESUME_NEXT;
  return 0;
}

// Commit a fully prepared replacement image for the current process. The
// caller retains ownership of pagetable if validation fails.
int
proc_exec(pagetable_t pagetable, uint64 sz, uint64 entry, uint64 stack,
          uint64 argc, uint64 argv)
{
  struct trapframe next;
  struct proc *p = myproc();
  pagetable_t oldpagetable;
  uint64 oldsz;

  if (p == 0 || p->state != RUNNING || p->pagetable == 0 || p->trapframe == 0 ||
      pagetable == 0 || pagetable == p->pagetable ||
      MMIX_RV_N(pagetable->rv) != MMIX_RV_N(p->pagetable->rv) ||
      sz < MMIX_USER_IMAGE_BASE || sz > MMIX_USER_HEAP_LIMIT || entry >= sz ||
      proc_user_context(pagetable, &next, entry, stack, argc, argv) < 0)
    return -1;

  oldpagetable = p->pagetable;
  oldsz = p->sz;
  acquire(&p->lock);
  if (p->state != RUNNING || p->vm_owner_cpu != cpuid() ||
      p->pagetable != oldpagetable) {
    release(&p->lock);
    return -1;
  }
  p->pagetable = pagetable;
  p->sz = sz;
  p->lazy_start = 0;
  *p->trapframe = next;
  proc_vm_mutated(p);
  release(&p->lock);
  proc_freepagetable(oldpagetable, oldsz);
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
      ((parent->lazy_start == 0) !=
       (MMIX_RV_F(parent->pagetable->rv) == MMIX_RV_F_HARDWARE)) ||
      (parent->lazy_start != 0 &&
       (parent->lazy_start < MMIX_USER_IMAGE_BASE ||
        parent->lazy_start > parent->sz)) ||
      parent->trapframe->user_rv != parent->pagetable->rv ||
      parent->trapframe->user_rk != MMIX_PROC_USER_RK)
    return 0;

  child = proc_user_alloc_internal(kernel_entry, 0);
  if (child == 0)
    return 0;
  if (uvmcopy(parent->pagetable, child->pagetable, parent->sz) < 0)
    goto fail;

  child->sz = parent->sz;
  child->lazy_start = parent->lazy_start;
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

static void proc_user_run(void) __attribute__((noreturn));

static void
proc_user_run(void)
{
  for (;;) {
    usertrapret();
    usertrap();
  }
}

static void
proc_user_entry(void)
{
  struct proc *p = myproc();

  if (p == 0 || !holding(&p->lock) || p->state != RUNNING)
    panic("user process entry");
  release(&p->lock);
  proc_user_run();
}

// The first process performs disk-backed initialization from a schedulable
// kernel context, then replaces its empty image with /init and enters user
// space through the same path used by every later process.
static void
proc_init_entry(void)
{
  char *argv[] = {"init", 0};
  struct proc *p = myproc();

  if (p == 0 || p != initproc || !holding(&p->lock) || p->state != RUNNING)
    panic("init process entry");
  release(&p->lock);

  fsinit(ROOTDEV);
  p->cwd = namei("/");
  if (p->cwd == 0)
    panic("init cwd");
  if (kexec("/init", argv) < 0)
    panic("exec init");
  proc_user_run();
}

// Set up the first process without embedding a bootstrap image in the kernel.
// Its scheduler-owned entry performs operations that may sleep, including log
// recovery and loading /init from the file system.
void
userinit(void)
{
  struct proc *p = proc_user_alloc_internal(proc_init_entry, 0);

  if (p == 0)
    panic("userinit");
  initproc = p;
  safestrcpy(p->name, "init", sizeof(p->name));
  proc_start(p);
}

int
kfork(void)
{
  struct proc *parent = myproc();
  struct proc *child;
  int pid;

  if (parent == 0)
    return -1;

  child = proc_user_clone(parent, proc_user_entry);
  if (child == 0)
    return -1;
  for (int fd = 0; fd < NOFILE; fd++)
    if (parent->ofile[fd] != 0)
      child->ofile[fd] = filedup(parent->ofile[fd]);
  if (parent->cwd != 0)
    child->cwd = idup(parent->cwd);
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

  if (p == 0)
    return -1;
  acquire(&p->lock);
  if (p->state != RUNNING || p->vm_owner_cpu != cpuid() ||
      p->pagetable == 0 || p->trapframe == 0 ||
      p->sz < MMIX_USER_IMAGE_BASE)
    goto fail;
  oldsz = p->sz;
  if (n > 0) {
    if ((uint64)n > MMIX_USER_HEAP_LIMIT - oldsz)
      goto fail;
    newsz = oldsz + (uint64)n;
    if (uvmalloc(p->pagetable, oldsz, newsz, PTE_W) == 0)
      goto fail;
  } else if (n < 0) {
    // Match xv6: an unsigned underflow is a successful no-op in uvmdealloc().
    newsz = oldsz + (long)n;
    if (newsz < MMIX_USER_IMAGE_BASE)
      goto fail;
    newsz = uvmdealloc(p->pagetable, oldsz, newsz);
    if (p->lazy_start != 0 && newsz <= p->lazy_start) {
      // No sparse interval remains, so hardware walks are sufficient again.
      p->lazy_start = 0;
      p->pagetable->rv =
        mmix_user_rv_set_function(p->pagetable->rv, MMIX_RV_F_HARDWARE);
      p->trapframe->user_rv = p->pagetable->rv;
    }
  } else {
    release(&p->lock);
    return 0;
  }
  p->sz = newsz;
  proc_vm_mutated(p);
  release(&p->lock);
  return 0;

fail:
  release(&p->lock);
  return -1;
}

int
growproc(int n)
{
  return proc_user_grow(myproc(), n);
}

int
cpuid(void)
{
  uint64 id = mmix_ru_cpu_id(mmix_ru_read());

  if (id >= NCPU)
    panic("cpu identity");
  return (int)id;
}

// Callers keep dynamic interrupts masked while using CPU-local state.
struct cpu *
mycpu(void)
{
  return &cpus[cpuid()];
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

static void scheduler_loop(void) __attribute__((noreturn));

// Enter the per-CPU scheduler on its dedicated MMIX context.
void
scheduler(void)
{
  struct context startup_context;
  struct cpu *c;
  uint slot;

  intr_off();
  c = mycpu();
  slot = MMIX_CONTEXT_SCHEDULER_SLOT(cpuid());
  if (kcontext_current_valid(&c->context, slot))
    scheduler_loop();
  if (c->context.state != 0)
    panic("scheduler context");
  kcontext_prepare(&c->context, slot, scheduler_loop);
  swtch(&startup_context, &c->context);
  panic("scheduler returned");
}

// A process switches out holding p->lock; this loop releases that lock only
// after the scheduler context is restored.
static void
scheduler_loop(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  if (c->scheduler_entries != 0)
    panic("scheduler entry");
  c->scheduler_entries = 1;
  if (boot_publish_scheduler_ready() < 0)
    panic("scheduler publish");
  c->proc = 0;
  for (;;) {
    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      // No process or process stack is owned here. Enable timer delivery before
      // acquire() closes the state-transition and context-switch window.
      intr_on();
      acquire(&p->lock);
      if (p->state == RUNNABLE &&
          (p->resume_cpu == -1 || p->resume_cpu == cpuid())) {
        proc_vm_claim_locked(p, c);
        p->state = RUNNING;
        c->proc = p;
        if (c->scheduler_dispatches == ~0ULL)
          panic("scheduler dispatch");
        c->scheduler_dispatches++;
        swtch(&c->context, &p->context);

        if (!holding(&p->lock) || c->noff != 1 || intr_get() ||
            p->state == RUNNING)
          panic("scheduler state");
        if (p->state == UNUSED) {
          if (p->chan != 0 || !proc_user_state_empty(p))
            panic("scheduler release");
          memset(&p->context, 0, sizeof(p->context));
        }
        proc_vm_release_locked(p, c);
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
static void
yield_to_cpu(int resume_cpu)
{
  struct proc *p = myproc();

  if (p == 0 || resume_cpu < -1 || resume_cpu >= NCPU)
    panic("yield proc");
  acquire(&p->lock);
  if (p->state != RUNNING || p->resume_cpu != -1)
    panic("yield state");
  p->resume_cpu = resume_cpu;
  p->state = RUNNABLE;
  sched();
  if (resume_cpu != -1) {
    if (p->resume_cpu != cpuid())
      panic("yield CPU");
    p->resume_cpu = -1;
  }
  release(&p->lock);
}

void
yield(void)
{
  yield_to_cpu(-1);
}

void
yield_pinned(void)
{
  yield_to_cpu(cpuid());
}

static void
reparent(struct proc *p)
{
  for (struct proc *child = proc; child < &proc[NPROC]; child++) {
    if (child->parent == p) {
      child->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process without returning through its user continuation.
// Its address space remains owned by the ZOMBIE until its parent reaps it.
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("exit proc");
  if (p == initproc)
    panic("init exiting");
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] != 0) {
      struct file *f = p->ofile[fd];

      p->ofile[fd] = 0;
      fileclose(f);
    }
  }
  if (p->cwd != 0) {
    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;
  }

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

// Print a process listing to console. For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
    // clang-format off
    [UNUSED] =   "unused",
    [USED] =     "used",
    [SLEEPING] = "sleep ",
    [RUNNABLE] = "runble",
    [RUNNING] =  "run   ",
    [ZOMBIE] =   "zombie"
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\n");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printk("%d %s %s", p->pid, state, p->name);
    printk("\n");
  }
}

_Static_assert(NCPU == MMIX_MAX_CPUS,
               "CPU table must cover the supported MMIX topology");
