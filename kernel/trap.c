#include "memlayout.h"
#include "mmix.h"
#include "cpu.h"
#include "spinlock.h"
#include "proc.h"
#include "kalloc.h"
#include "diagnostic.h"
#include "intc.h"
#include "timer.h"
#include "defs.h"

extern char kernel_text_end[];
extern char mmix_kernel_trap_entry[];
extern void mmix_user_resume(void);

volatile uint64 mmix_trap_active;
uint64 mmix_trap_vector;
volatile uint64 mmix_user_trapframe;
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
  mmix_trap_active = 0;
  yield();
  if (mmix_trap_active != 0)
    trap_stop(MMIX_TRAP_EXTERNAL, "preemption active", state, claim);
  // A voluntary scheduler path may leave only the program mask enabled.
  // Re-enter the suspended dynamic trap with the hardware mask cleared.
  mmix_intr_mask_write(0);
  if (mmix_rk_read() != 0)
    trap_stop(MMIX_TRAP_EXTERNAL, "preemption mask", state, claim);
  mmix_trap_rk_shadow = state->restore_rk;
  mmix_trap_active = 1;
}

static const char *
trap_device_service(uint64 rq, uint64 restore_rk, uint64 rxx, uint32 *claim,
                    int *preempt)
{
  int pending;
  int status;

  *claim = 0;
  *preempt = 0;
  if (!mmix_rq_intc_pending(rq, restore_rk))
    return "masked request";
  if (rxx != MMIX_DYNAMIC_TRAP_RESUME_NEXT)
    return "unsupported resume";

  status = intc_claim(claim);
  if (status == MMIX_INTC_NO_IRQ)
    return "zero claim";
  if (status != MMIX_INTC_OK)
    return "invalid claim";
  if (*claim == UART0_IRQ) {
    uartintr();
    if (intc_complete(*claim) != MMIX_INTC_OK)
      return "controller complete";
    return 0;
  }
  if (*claim == VIRTIO0_IRQ) {
    virtio_disk_intr();
    if (intc_complete(*claim) != MMIX_INTC_OK)
      return "controller complete";
    return 0;
  }
  if (*claim != MMIX_TIMER_IRQ)
    return "unexpected claim";
  if (timer_pending(&pending) != MMIX_TIMER_OK || !pending)
    return "timer not pending";

  if (timer_arm_next() != MMIX_TIMER_OK)
    return "timer rearm";
  if (timer_acknowledge() != MMIX_TIMER_OK)
    return "timer acknowledge";
  if (intc_complete(*claim) != MMIX_INTC_OK)
    return "controller complete";
  if (timer_record_tick() != MMIX_TIMER_OK)
    return "tick overflow";
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
  *preempt = 1;

  return 0;
}

static void
trap_external(struct mmix_trap_state *state)
{
  uint32 irq;
  int preempt;
  const char *error;

  error = trap_device_service(state->rq, state->restore_rk, state->rxx, &irq,
                              &preempt);
  if (error != 0)
    trap_stop(MMIX_TRAP_EXTERNAL, error, state, irq);

  // GET/PUT rQ is a request handoff. Do not restore the serviced controller
  // bit or RESUME would immediately deliver the same dynamic trap again.
  state->rq &= ~MMIX_RQ_INTC;

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
  pte = vmfault(p->pagetable, trapframe->ryy, permissions);
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
  // Forced translation entry performed GET rQ just like every other user
  // entry, so complete that CPU-owned request handoff before returning.
  mmix_rq_write(trapframe->rq);
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
  struct proc *p = myproc();
  struct trapframe *trapframe;
  uint64 cause;

  mmix_intr_mask_write(0);
  if (p == 0 || p->state != RUNNING || holding(&p->lock) ||
      mmix_user_trapframe != 0 || mmix_trap_active != 0)
    panic("user trap owner");
  trapframe = p->trapframe;
  if (trapframe == 0 || trapframe->flags != MMIX_PROC_TRAPFRAME_READY ||
      trapframe->kernel_state != 0 || trapframe->reserved != 0 ||
      (trapframe->user_state & (sizeof(uint64) - 1)) != 0 ||
      trapframe->user_state < MMIX_USER_REGISTER_STACK_BASE ||
      trapframe->user_state >= MMIX_USER_REGISTER_STACK_TOP ||
      trapframe->user_rv != p->pagetable->rv ||
      trapframe->user_rk != MMIX_PROC_USER_RK ||
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
             mmix_rq_intc_pending(trapframe->rq, trapframe->user_rk)) {
    uint32 irq;
    int preempt;
    const char *error = trap_device_service(
      trapframe->rq, trapframe->user_rk, trapframe->rxx, &irq, &preempt);

    if (error != 0)
      user_trap_stop(error, p, irq);
    trapframe->rq &= ~MMIX_RQ_INTC;
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
  uint64 ro = mmix_ro_read();
  uint64 rs = mmix_rs_read();
  uint64 sp = mmix_sp_read();

  mmix_intr_mask_write(0);
  mmix_trap_active = 0;
  ticks = 0;
  initlock(&tickslock, "time");
  mmix_ra_write(mmix_ra_disable_trips(mmix_ra_read()));

  if (ro < REGISTER_STACK_BASE || ro >= REGISTER_STACK_LIMIT ||
      rs < REGISTER_STACK_BASE || rs >= REGISTER_STACK_LIMIT)
    panic("trap register stack");
  if (sp <= BOOT_STACK_BASE + MMIX_TRAP_STACK_RESERVE || sp > BOOT_STACK_TOP)
    panic("trap software stack");
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
  uint64 requests;

  mmix_intr_mask_write(0);
  if (mmix_trap_vector == 0 || mmix_trap_active != 0)
    panic("trap state");

  mmix_rt_write(mmix_trap_vector);
  mmix_rtt_write(mmix_trap_vector);
  if (mmix_rt_read() != mmix_trap_vector ||
      mmix_rtt_read() != mmix_trap_vector || mmix_rk_read() != 0)
    panic("trap install");

  requests = mmix_rq_read();
  mmix_rq_write(requests & ~MMIX_RQ_PROGRAM_MASK);
  mmix_intr_mask_write(MMIX_KERNEL_PROGRAM_MASK);
}

// Switch from the current process's kernel continuation to its saved user
// continuation. This call returns only after the next user trap has made the
// user state process-owned again.
void
usertrapret(void)
{
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  struct trapframe *trapframe;
  uint64 alias;
  uint32 enabled_irqs;

  mmix_intr_mask_write(0);
  if (p == 0 || p->state != RUNNING || holding(&p->lock) || c->noff != 0 ||
      mmix_user_trapframe != 0 || mmix_trap_active != 0)
    panic("user return owner");
  if (killed(p))
    kexit(-1);
  if (intc_enabled(&enabled_irqs) != MMIX_INTC_OK ||
      (enabled_irqs & (1U << MMIX_TIMER_IRQ)) == 0)
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

  alias = mmix_phys_alias((uint64)trapframe);
  trapframe->flags = MMIX_PROC_TRAPFRAME_ACTIVE;
  asm volatile("" : : : "memory");
  mmix_user_trapframe = alias;
  asm volatile("" : : : "memory");
  mmix_user_resume();
  asm volatile("" : : : "memory");

  if (c->proc != p || p->trapframe != trapframe ||
      mmix_user_trapframe != 0 || mmix_trap_active != 0 ||
      trapframe->kernel_state != 0 ||
      trapframe->flags != MMIX_PROC_TRAPFRAME_READY ||
      trapframe->reserved != 0 ||
      (trapframe->user_state & (sizeof(uint64) - 1)) != 0 ||
      trapframe->user_state < MMIX_USER_REGISTER_STACK_BASE ||
      trapframe->user_state >= MMIX_USER_REGISTER_STACK_TOP ||
      (trapframe->rww & MMIX_PHYSICAL_ALIAS_BIT) != 0 ||
      mmix_rv_read() != MMIX_KERNEL_RV || mmix_rk_read() != 0)
    panic("user trap state");
}

void
mmix_kernel_trap(enum mmix_trap_class event, struct mmix_trap_state *state)
{
  uint64 cause = state->rxx & MMIX_RQ_PROGRAM_MASK;

  if (mmix_trap_active != 1 || state->rg != MMIX_TRAP_GLOBAL_FIRST ||
      state->restore_rk != mmix_trap_rk_shadow || mmix_rk_read() != 0)
    trap_stop(event, "inconsistent state", state, 0);

  if (event == MMIX_TRAP_FORCED) {
    if (state->rxx ==
        (MMIX_DYNAMIC_TRAP_RESUME_NEXT | MMIX_KERNEL_FORCED_TRAP_INSN)) {
      trap_report(event, "expected", state, 0);
      mmix_trap_rk_shadow = state->restore_rk;
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
    mmix_trap_rk_shadow = state->restore_rk;
    return;
  }
  trap_stop(event, "unknown", state, 0);
}
