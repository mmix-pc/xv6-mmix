#include "memlayout.h"
#include "mmix.h"
#include "boot.h"
#include "cpu.h"
#include "spinlock.h"
#include "proc.h"
#include "kalloc.h"
#include "diagnostic.h"
#include "intc.h"
#include "ipi.h"
#include "platform.h"
#include "timer.h"
#include "defs.h"

extern char kernel_text_end[];
extern char mmix_kernel_trap_entry[];
extern void mmix_user_resume(void);

uint64 mmix_trap_vector;
// Kernel trap entry uses rV, not CPU-local user scratch, to classify traps.
uint ticks;
struct spinlock tickslock;

static void
trap_report(enum mmix_trap_class event, const char *cause,
            const struct mmix_trap_state *state, uint32 claim)
{
  struct proc *p = mycpu()->proc;
  const char *event_class = "unknown";
  uint32 enabled_irqs = ~(uint32)0;
  uint32 pending_irqs = ~(uint32)0;
  int timer_is_pending = -1;
  int ipi_is_pending = -1;
  uint64 ipi_received = ~0ULL;
  uint64 ipi_acknowledged_generation = ~0ULL;

  if (event == MMIX_TRAP_FORCED)
    event_class = "forced";
  else if (event == MMIX_TRAP_PROGRAM)
    event_class = "program";
  else if (event == MMIX_TRAP_EXTERNAL)
    event_class = "external";

  if (intc_enabled(&enabled_irqs) != MMIX_INTC_OK)
    enabled_irqs = ~(uint32)0;
  if (intc_pending(&pending_irqs) != MMIX_INTC_OK)
    pending_irqs = ~(uint32)0;
  if (timer_pending(&timer_is_pending) != MMIX_TIMER_OK)
    timer_is_pending = -1;
  if (ipi_pending(&ipi_is_pending) != MMIX_IPI_OK)
    ipi_is_pending = -1;
  if (ipi_progress(&ipi_received, &ipi_acknowledged_generation) != MMIX_IPI_OK) {
    ipi_received = ~0ULL;
    ipi_acknowledged_generation = ~0ULL;
  }

  struct mmix_trap_diagnostic diagnostic = {
    .from_user = 0,
    .event_class = event_class,
    .cause = cause,
    .pid = p == 0 ? 0 : p->pid,
    .process_name = p == 0 ? "-" : p->name,
    .rq = state->rq,
    .active_rk = mmix_rk_read(),
    .restore_rk = state->restore_rk,
    .rww = state->rww,
    .rxx = state->rxx,
    .ryy = state->ryy,
    .rzz = state->rzz,
    .state = (uint64)state,
    .sp = state->globals[254 - MMIX_TRAP_GLOBAL_FIRST],
    .fp = state->globals[253 - MMIX_TRAP_GLOBAL_FIRST],
    .ro = state->ro,
    .rs = state->rs,
    .rl = state->rl,
    .intc_pending = pending_irqs,
    .intc_enabled = enabled_irqs,
    .intc_claim = claim,
    .timer_pending = timer_is_pending,
    .ipi_pending = ipi_is_pending,
    .ipi_received = ipi_received,
    .ipi_acknowledged = ipi_acknowledged_generation,
  };

  diagnostic_trap(&diagnostic);
}

static void trap_stop(enum mmix_trap_class event, const char *cause,
                      struct mmix_trap_state *state, uint32 claim)
  __attribute__((noreturn));

static void
trap_stop(enum mmix_trap_class event, const char *cause,
          struct mmix_trap_state *state, uint32 claim)
{
  mmix_intr_mask_write(0);
  trap_report(event, cause, state, claim);

  for (;;)
    asm volatile("SWYM 0, 0, 0");
}

static void
trap_preempt(struct mmix_trap_state *state, uint32 claim)
{
  struct cpu *c = mycpu();
  struct proc *p = c->proc;

  // A scheduler or idle tick is accounted above but has no process to yield.
  if (p == 0)
    return;
  if (p->state != RUNNING || c->noff != 0)
    trap_stop(MMIX_TRAP_EXTERNAL, "unsafe preemption", state, claim);

  // This trap remains on p's stacks while yield() runs another process. Let
  // that process take traps, then reclaim the single-CPU trap entry on return.
  // Complete the rQ handoff now because the assembly restore is also suspended.
  mmix_rq_write(state->rq);
  c->trap.active = 0;
  yield_pinned();
  if (c->trap.active != 0)
    trap_stop(MMIX_TRAP_EXTERNAL, "preemption active", state, claim);
  // A voluntary scheduler path may leave only the program mask enabled.
  // Re-enter the suspended dynamic trap with the hardware mask cleared.
  mmix_intr_mask_write(0);
  if (mmix_rk_read() != 0)
    trap_stop(MMIX_TRAP_EXTERNAL, "preemption mask", state, claim);
  c->trap.rk_shadow = state->restore_rk;
  c->trap.active = 1;
}

static const char *
trap_interrupt_dispatch(uint64 rq, uint64 restore_rk, uint64 rxx,
                        uint64 *serviced, uint32 *claim, int *preempt)
{
  int pending;
  int status;
  int cpu_id = cpuid();
  uint32 shared_owner;
  uint32 timer_irq_number;

  *serviced = 0;
  *claim = 0;
  *preempt = 0;
  if (!mmix_rq_interrupt_pending(rq, restore_rk))
    return "masked request";
  if (rxx != MMIX_DYNAMIC_TRAP_RESUME_NEXT)
    return "unsupported resume";

  if (mmix_rq_ipi_pending(rq, restore_rk) &&
      !mmix_rq_intc_pending(rq, restore_rk)) {
    if (ipi_service(proc_vm_ipi_work) != MMIX_IPI_OK)
      return "IPI service";
    *serviced = MMIX_RQ_IPI;
    return 0;
  }

  status = intc_claim(claim);
  if (status == MMIX_INTC_NO_IRQ)
    return "zero claim";
  if (status != MMIX_INTC_OK)
    return "invalid claim";
  *serviced = MMIX_RQ_INTC;
  if (*claim == UART0_IRQ) {
    if (intc_shared_owner(*claim, &shared_owner) != MMIX_INTC_OK ||
        cpu_id != (int)shared_owner)
      return "foreign claim";
    uartintr();
    if (intc_complete(*claim) != MMIX_INTC_OK)
      return "controller complete";
    return 0;
  }
  if (*claim == VIRTIO0_IRQ) {
    if (intc_shared_owner(*claim, &shared_owner) != MMIX_INTC_OK ||
        cpu_id != (int)shared_owner)
      return "foreign claim";
    virtio_disk_intr();
    if (intc_complete(*claim) != MMIX_INTC_OK)
      return "controller complete";
    return 0;
  }
  if (timer_irq(&timer_irq_number) != MMIX_TIMER_OK)
    return "timer context";
  if (*claim != timer_irq_number)
    return "unexpected claim";
  if (timer_pending(&pending) != MMIX_TIMER_OK || !pending)
    return "timer not pending";

  if (timer_disable() != MMIX_TIMER_OK)
    return "timer disable";
  if (timer_acknowledge() != MMIX_TIMER_OK)
    return "timer acknowledge";
  if (timer_arm_next() != MMIX_TIMER_OK)
    return "timer rearm";
  if (intc_complete(*claim) != MMIX_INTC_OK)
    return "controller complete";
  if (timer_record_tick() != MMIX_TIMER_OK)
    return "tick overflow";
  if (cpu_id == BOOT_CPU_ID) {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }
  *preempt = 1;

  return 0;
}

static const char *
trap_interrupt_service(uint64 rq, uint64 restore_rk, uint64 rxx,
                       uint64 *serviced, uint32 *claim, int *preempt)
{
  struct cpu *c = mycpu();
  const char *error;
  uint64 entries;
  uint64 returns;

  entries = __atomic_load_n(&c->trap.interrupt_entries, __ATOMIC_RELAXED);
  if (entries == ~0ULL)
    return "interrupt entry overflow";
  __atomic_store_n(&c->trap.interrupt_entries, entries + 1,
                   __ATOMIC_RELEASE);
  error = trap_interrupt_dispatch(rq, restore_rk, rxx, serviced, claim,
                                  preempt);
  if (error != 0)
    return error;
  returns = __atomic_load_n(&c->trap.interrupt_returns, __ATOMIC_RELAXED);
  if (returns == ~0ULL || returns + 1 != entries + 1)
    return "interrupt return imbalance";
  __atomic_store_n(&c->trap.interrupt_returns, returns + 1,
                   __ATOMIC_RELEASE);
  return 0;
}

static void
trap_external(struct mmix_trap_state *state)
{
  uint32 irq;
  int preempt;
  uint64 serviced;
  const char *error;

  error = trap_interrupt_service(state->rq, state->restore_rk, state->rxx,
                                 &serviced, &irq, &preempt);
  if (error != 0)
    trap_stop(MMIX_TRAP_EXTERNAL, error, state, irq);

  // GET/PUT rQ is a request handoff. Preserve any independent source that
  // arrived with the one serviced by this entry.
  state->rq &= ~serviced;

  if (preempt)
    trap_preempt(state, irq);
}

static void
trapframe_report(int from_user, const char *event_class, const char *cause,
                 struct proc *p, uint32 claim)
{
  struct trapframe *trapframe = p->trapframe;
  uint64 fp = 0;
  uint64 sp = 0;
  uint64 address = trapframe->user_state +
                   MMIX_SAVED_GLOBAL_OFFSET(MMIX_ABI_GLOBAL_FIRST);
  uint32 enabled_irqs = ~(uint32)0;
  uint32 pending_irqs = ~(uint32)0;
  int timer_is_pending = -1;
  int ipi_is_pending = -1;
  uint64 ipi_received = ~0ULL;
  uint64 ipi_acknowledged_generation = ~0ULL;

  copyin(p->pagetable, (char *)&fp,
         address + (MMIX_ABI_FP - MMIX_ABI_GLOBAL_FIRST) * sizeof(uint64),
         sizeof(fp));
  copyin(p->pagetable, (char *)&sp,
         address + (MMIX_ABI_SP - MMIX_ABI_GLOBAL_FIRST) * sizeof(uint64),
         sizeof(sp));
  if (intc_enabled(&enabled_irqs) != MMIX_INTC_OK)
    enabled_irqs = ~(uint32)0;
  if (intc_pending(&pending_irqs) != MMIX_INTC_OK)
    pending_irqs = ~(uint32)0;
  if (timer_pending(&timer_is_pending) != MMIX_TIMER_OK)
    timer_is_pending = -1;
  if (ipi_pending(&ipi_is_pending) != MMIX_IPI_OK)
    ipi_is_pending = -1;
  if (ipi_progress(&ipi_received, &ipi_acknowledged_generation) != MMIX_IPI_OK) {
    ipi_received = ~0ULL;
    ipi_acknowledged_generation = ~0ULL;
  }

  struct mmix_trap_diagnostic diagnostic = {
    .from_user = from_user,
    .event_class = event_class,
    .cause = cause,
    .pid = p->pid,
    .process_name = p->name,
    .rq = trapframe->rq,
    .active_rk = mmix_rk_read(),
    .restore_rk = trapframe->user_rk,
    .rww = trapframe->rww,
    .rxx = trapframe->rxx,
    .ryy = trapframe->ryy,
    .rzz = trapframe->rzz,
    .state = trapframe->user_state,
    .sp = sp,
    .fp = fp,
    .ro = 0,
    .rs = 0,
    .rl = 0,
    .intc_pending = pending_irqs,
    .intc_enabled = enabled_irqs,
    .intc_claim = claim,
    .timer_pending = timer_is_pending,
    .ipi_pending = ipi_is_pending,
    .ipi_received = ipi_received,
    .ipi_acknowledged = ipi_acknowledged_generation,
  };

  diagnostic_trap(&diagnostic);
}

static void
user_trap_report(const char *cause, struct proc *p, uint32 claim)
{
  trapframe_report(1, "user", cause, p, claim);
}

static void
user_trap_stop(const char *cause, struct proc *p, uint32 claim)
  __attribute__((noreturn));

static void
user_trap_stop(const char *cause, struct proc *p, uint32 claim)
{
  mmix_intr_mask_write(0);
  trapframe_report(0, "external", cause, p, claim);
  for (;;)
    asm volatile("SWYM 0, 0, 0");
}

static const char *
user_program_cause(uint64 cause)
{
  if (cause & MMIX_RQ_PROGRAM_R)
    return "read fault";
  if (cause & MMIX_RQ_PROGRAM_W)
    return "write fault";
  if (cause & MMIX_RQ_PROGRAM_X)
    return "execute fault";
  if (cause & MMIX_RQ_PROGRAM_N)
    return "negative address";
  if (cause & MMIX_RQ_PROGRAM_K)
    return "privileged operation";
  if (cause & MMIX_RQ_PROGRAM_B)
    return "illegal instruction";
  if (cause & MMIX_RQ_PROGRAM_S)
    return "security violation";
  if (cause & MMIX_RQ_PROGRAM_P)
    return "negative instruction";
  return "unknown cause";
}

static int
user_translation_permissions(uint64 rxx)
{
  enum {
    MMIX_OPCODE_LOAD_FIRST = 0x80,
    MMIX_OPCODE_LOAD_LAST = 0x93,
    MMIX_OPCODE_CSWAP_FIRST = 0x94,
    MMIX_OPCODE_CSWAP_LAST = 0x95,
    MMIX_OPCODE_LOAD_UNCACHED_FIRST = 0x96,
    MMIX_OPCODE_LOAD_UNCACHED_LAST = 0x97,
    MMIX_OPCODE_STORE_FIRST = 0xa0,
    MMIX_OPCODE_STORE_LAST = 0xb7,
  };
  uint instruction;
  uint opcode;

  if ((rxx & MMIX_FORCED_TRANSLATION_MASK) !=
      MMIX_FORCED_TRANSLATION_PREFIX)
    return -1;
  instruction = (uint)(rxx & MMIX_FORCED_TRANSLATION_INSN_MASK);
  if (instruction == MMIX_SWYM_INSN)
    return PTE_X;
  opcode = instruction >> 24;
  if ((opcode >= MMIX_OPCODE_LOAD_FIRST &&
       opcode <= MMIX_OPCODE_LOAD_LAST) ||
      (opcode >= MMIX_OPCODE_LOAD_UNCACHED_FIRST &&
       opcode <= MMIX_OPCODE_LOAD_UNCACHED_LAST))
    return PTE_R;
  if ((opcode >= MMIX_OPCODE_CSWAP_FIRST &&
       opcode <= MMIX_OPCODE_CSWAP_LAST) ||
      (opcode >= MMIX_OPCODE_STORE_FIRST &&
       opcode <= MMIX_OPCODE_STORE_LAST))
    return PTE_R | PTE_W;

  // Implicit register-stack accesses retain the interrupted instruction's
  // opcode. Offer only an existing mapping and let RESUME validate its use.
  return 0;
}

static int
user_translation_trap(struct proc *p)
{
  struct trapframe *trapframe = p->trapframe;
  int permissions = user_translation_permissions(trapframe->rxx);
  uint64 pte;

  if (permissions < 0)
    return 0;
  // Complete the user-owned rQ handoff before allowing a lazy allocation to
  // sleep while it serializes with another mapping mutation.
  mmix_rq_write(trapframe->rq);
  mmix_intr_mask_write(MMIX_KERNEL_TRAP_MASK);
  pte = vmfault(p->pagetable, trapframe->ryy, permissions);
  mmix_intr_mask_write(0);
  if (pte == 0) {
    const char *cause = permissions == PTE_X ? "execute fault" :
                        permissions == PTE_R ? "read fault" :
                        permissions == (PTE_R | PTE_W) ? "write fault" :
                        "translation fault";

    user_trap_report(cause, p, 0);
    setkilled(p);
  } else {
    trapframe->rzz = pte;
  }
  return 1;
}

static int
user_syscall_trap(struct proc *p)
{
  struct trapframe *trapframe = p->trapframe;
  uint instruction = (uint)trapframe->rxx;
  uint64 origin;
  pte_t *pte;

  if ((trapframe->rxx & MMIX_DYNAMIC_TRAP_RESUME_NEXT) == 0 ||
      (instruction & 0xff00ffff) != 0 ||
      ((instruction >> 16) & 0xff) == 0 ||
      trapframe->rww < MMIX_USER_IMAGE_BASE + sizeof(uint) ||
      trapframe->rww >= MMIX_USER_HEAP_LIMIT ||
      (trapframe->rww & (sizeof(uint) - 1)) != 0)
    return 0;
  origin = trapframe->rww - sizeof(uint);
  pte = walk(p->pagetable, origin, 0);
  return pte != 0 && kalloc_page_is_managed((void *)mmix_pte_pa(*pte)) &&
         *pte == mmix_pte_make(mmix_pte_pa(*pte),
                               MMIX_RV_N(p->pagetable->rv),
                               mmix_pte_permissions(*pte)) &&
         (mmix_pte_permissions(*pte) & PTE_X) != 0;
}

// Dispatch one complete trap saved by the MMIX user-entry assembly. Syscall
// decoding and execution are layered onto the accepted forced-TRAP case.
void
usertrap(void)
{
  struct cpu *c = mycpu();
  struct proc *p = myproc();
  struct trapframe *trapframe;
  uint64 cause;

  mmix_intr_mask_write(0);
  if (p == 0 || p->state != RUNNING || holding(&p->lock) ||
      p->vm_owner_cpu != cpuid() || c->trap.user_trapframe != 0 ||
      c->trap.active != 0)
    panic("user trap owner");
  trapframe = p->trapframe;
  if (trapframe == 0 || p->pagetable == 0 ||
      trapframe->flags != MMIX_PROC_TRAPFRAME_READY ||
      trapframe->kernel_state != 0 || trapframe->reserved != 0 ||
      (trapframe->user_state & (sizeof(uint64) - 1)) != 0 ||
      trapframe->user_state < MMIX_USER_REGISTER_STACK_BASE ||
      trapframe->user_state >= MMIX_USER_REGISTER_STACK_TOP ||
      trapframe->user_rv != p->pagetable->rv ||
      trapframe->user_rk != MMIX_PROC_USER_RK ||
      mmix_ru_cpu_id(trapframe->user_ru) != (uint64)cpuid() ||
      mmix_rv_read() != MMIX_KERNEL_RV || mmix_rk_read() != 0)
    panic("user trap state");

  cause = trapframe->rxx & MMIX_RQ_PROGRAM_MASK;
  if (cause != 0) {
    const char *name = user_program_cause(cause);

    user_trap_report(name, p, 0);
    trapframe->rq &= ~MMIX_RQ_PROGRAM_MASK;
    mmix_rq_write(trapframe->rq);
    setkilled(p);
  } else if (user_translation_trap(p)) {
    // RESUME 1 installs rZZ and retries the instruction that missed.
  } else if (trapframe->rxx == MMIX_DYNAMIC_TRAP_RESUME_NEXT &&
             mmix_rq_interrupt_pending(trapframe->rq,
                                       trapframe->user_rk)) {
    uint32 irq;
    int preempt;
    uint64 serviced;
    const char *error = trap_interrupt_service(
      trapframe->rq, trapframe->user_rk, trapframe->rxx, &serviced, &irq,
      &preempt);

    if (error != 0)
      user_trap_stop(error, p, irq);
    trapframe->rq &= ~serviced;
    // The GET performed by user entry is CPU state, not process state. Finish
    // its handoff before another scheduled context can enable dynamic traps.
    mmix_rq_write(trapframe->rq);
    if (killed(p))
      kexit(-1);
    if (preempt)
      yield();
  } else {
    int is_syscall = user_syscall_trap(p);

    if (!is_syscall) {
      user_trap_report("unexpected trap", p, 0);
      setkilled(p);
    }
    // Forced entry also performed GET rQ. Finish the CPU-owned handoff even
    // when the process exits instead of reaching the resume assembly.
    mmix_rq_write(trapframe->rq);
    if (is_syscall) {
      if (killed(p))
        kexit(-1);
      mmix_intr_mask_write(MMIX_KERNEL_TRAP_MASK);
      syscall();
      mmix_intr_mask_write(0);
    }
  }

  if (killed(p))
    kexit(-1);
}

void
trapinit(void)
{
  uint64 entry = (uint64)mmix_kernel_trap_entry;

  mmix_intr_mask_write(0);
  ticks = 0;
  initlock(&tickslock, "time");
  if (mmix_trap_vector_make(entry, (uint64)kernel_text_end, &mmix_trap_vector) <
      0)
    panic("trap vector");

  // rT/rTT will execute the same bytes through the privileged physical alias.
  if (*(volatile uint *)entry != *(volatile uint *)mmix_trap_vector)
    panic("trap alias");
}

void
trapinithart(void)
{
  struct cpu *c = mycpu();
  uint64 cpu_id = cpuid();
  uint64 requests;
  uint64 ro = mmix_ro_read();
  uint64 rs = mmix_rs_read();
  uint64 sp = mmix_sp_read();

  mmix_rk_write(0);
  c->trap.active = 0;
  c->trap.rk_shadow = 0;
  c->trap.interrupt_entries = 0;
  c->trap.interrupt_returns = 0;
  c->trap.user_trapframe = 0;
  if (mmix_trap_vector == 0)
    panic("trap state");
  if (!platform_cpu_initial_stack_contains(cpu_id, ro) ||
      !platform_cpu_initial_stack_contains(cpu_id, rs))
    panic("trap register stack");
  if (sp <= boot_stack_base(cpu_id) + MMIX_TRAP_STACK_RESERVE ||
      sp > boot_stack_top(cpu_id))
    panic("trap software stack");

  mmix_ra_write(mmix_ra_disable_trips(mmix_ra_read()));
  mmix_rt_write(mmix_trap_vector);
  mmix_rtt_write(mmix_trap_vector);
  if (mmix_rt_read() != mmix_trap_vector ||
      mmix_rtt_read() != mmix_trap_vector || mmix_rk_read() != 0)
    panic("trap install");

  requests = mmix_rq_read();
  mmix_rq_write(requests & ~MMIX_RQ_PROGRAM_MASK);
}

void
trapenablehart(void)
{
  if (mmix_rt_read() != mmix_trap_vector ||
      mmix_rtt_read() != mmix_trap_vector || mmix_rk_read() != 0)
    panic("trap enable");
  mmix_intr_mask_write(MMIX_KERNEL_PROGRAM_MASK);
}

// Switch from the current process's kernel continuation to its saved user
// continuation. This call returns only after the next user trap has made the
// user state process-owned again.
void
usertrapret(void)
{
  struct cpu *c;
  struct proc *p;
  struct trapframe *trapframe;
  uint64 alias;
  uint32 enabled_irqs;
  uint32 timer_irq_number;

  mmix_intr_mask_write(0);
  c = mycpu();
  p = c->proc;
  if (p == 0 || p->state != RUNNING || p->vm_owner_cpu != cpuid() ||
      holding(&p->lock) || c->noff != 0 || c->trap.user_trapframe != 0 ||
      c->trap.active != 0)
    panic("user return owner");
  if (killed(p))
    kexit(-1);
  if (timer_irq(&timer_irq_number) != MMIX_TIMER_OK ||
      intc_enabled(&enabled_irqs) != MMIX_INTC_OK ||
      (enabled_irqs & (1U << timer_irq_number)) == 0)
    panic("user return timer");
  trapframe = p->trapframe;
  if (trapframe == 0 || !kalloc_page_is_managed(trapframe) ||
      ((uint64)trapframe & (PGSIZE - 1)) != 0 || p->pagetable == 0 ||
      trapframe->user_rv != p->pagetable->rv ||
      trapframe->user_rk != MMIX_PROC_USER_RK ||
      trapframe->kernel_state != 0 ||
      trapframe->flags != MMIX_PROC_TRAPFRAME_READY ||
      trapframe->reserved != 0 ||
      (trapframe->user_state & (sizeof(uint64) - 1)) != 0 ||
      trapframe->user_state < MMIX_USER_REGISTER_STACK_BASE ||
      trapframe->user_state >= MMIX_USER_REGISTER_STACK_TOP)
    panic("user return state");

  if (mmix_rv_read() != MMIX_KERNEL_RV ||
      mmix_sp_read() <= p->kstack || mmix_sp_read() > p->kstack + PGSIZE ||
      mmix_ro_read() < p->kstack + 2 * PGSIZE ||
      mmix_ro_read() >= p->kstack + 3 * PGSIZE ||
      mmix_rs_read() < p->kstack + 2 * PGSIZE ||
      mmix_rs_read() >= p->kstack + 3 * PGSIZE)
    panic("user return stack");

  proc_vm_prepare_user(p);

  alias = mmix_phys_alias((uint64)trapframe);
  // Preserve the user usage mask and count, but never its CPU selector.
  trapframe->user_ru = mmix_ru_bind_cpu(trapframe->user_ru, cpuid());
  trapframe->flags = MMIX_PROC_TRAPFRAME_ACTIVE;
  asm volatile("" : : : "memory");
  c->trap.user_trapframe = alias;
  asm volatile("" : : : "memory");
  mmix_user_resume();
  asm volatile("" : : : "memory");

  if (c->proc != p || p->trapframe != trapframe ||
      c->trap.user_trapframe != 0 || c->trap.active != 0 ||
      trapframe->kernel_state != 0 ||
      trapframe->flags != MMIX_PROC_TRAPFRAME_READY ||
      trapframe->reserved != 0 ||
      (trapframe->user_state & (sizeof(uint64) - 1)) != 0 ||
      trapframe->user_state < MMIX_USER_REGISTER_STACK_BASE ||
      trapframe->user_state >= MMIX_USER_REGISTER_STACK_TOP ||
      (trapframe->rww & MMIX_PHYSICAL_ALIAS_BIT) != 0 ||
      mmix_ru_cpu_id(trapframe->user_ru) != (uint64)cpuid() ||
      mmix_rv_read() != MMIX_KERNEL_RV || mmix_rk_read() != 0)
    panic("user trap state");
}

void
mmix_kernel_trap(enum mmix_trap_class event, struct mmix_trap_state *state)
{
  struct cpu *c = mycpu();
  uint64 cause = state->rxx & MMIX_RQ_PROGRAM_MASK;

  if (c->trap.active != 1 || state->rg != MMIX_TRAP_GLOBAL_FIRST ||
      state->restore_rk != c->trap.rk_shadow || mmix_rk_read() != 0)
    trap_stop(event, "inconsistent state", state, 0);

  if (event == MMIX_TRAP_FORCED) {
    if (state->rxx ==
        (MMIX_DYNAMIC_TRAP_RESUME_NEXT | MMIX_KERNEL_FORCED_TRAP_INSN)) {
      trap_report(event, "expected", state, 0);
      c->trap.rk_shadow = state->restore_rk;
      return;
    }
    trap_stop(event, "unexpected instruction", state, 0);
  }

  if (event == MMIX_TRAP_PROGRAM) {
    if (cause & MMIX_RQ_PROGRAM_R)
      trap_stop(event, "read fault", state, 0);
    if (cause & MMIX_RQ_PROGRAM_W)
      trap_stop(event, "write fault", state, 0);
    if (cause & MMIX_RQ_PROGRAM_X)
      trap_stop(event, "execute fault", state, 0);
    if (cause & MMIX_RQ_PROGRAM_B)
      trap_stop(event, "illegal instruction", state, 0);
    trap_stop(event, "unknown cause", state, 0);
  }

  if (event == MMIX_TRAP_EXTERNAL) {
    trap_external(state);
    // Interrupt-side locks temporarily publish a zero mask. Restore the
    // interrupted mask before RESUME can immediately deliver another source.
    c->trap.rk_shadow = state->restore_rk;
    return;
  }
  trap_stop(event, "unknown", state, 0);
}
