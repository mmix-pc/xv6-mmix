#include "memlayout.h"
#include "mmix.h"
#include "early_print.h"
#include "intc.h"
#include "timer.h"

extern char kernel_text_end[];
extern char mmix_kernel_trap_entry[];

volatile uint64 mmix_trap_active;
uint64 mmix_trap_vector;

static void
trap_report(enum mmix_trap_class event, const char *cause,
            const struct mmix_trap_state *state, uint32 claim)
{
  const char *event_class = "unknown";
  uint32 intc_enabled = ~(uint32)0;
  uint32 intc_pending = ~(uint32)0;
  int timer_pending = -1;

  if (event == MMIX_TRAP_FORCED)
    event_class = "forced";
  else if (event == MMIX_TRAP_PROGRAM)
    event_class = "program";
  else if (event == MMIX_TRAP_EXTERNAL)
    event_class = "external";

  if (mmix_intc_enabled(&intc_enabled) != MMIX_INTC_OK)
    intc_enabled = ~(uint32)0;
  if (mmix_intc_pending(&intc_pending) != MMIX_INTC_OK)
    intc_pending = ~(uint32)0;
  if (mmix_timer_pending(&timer_pending) != MMIX_TIMER_OK)
    timer_pending = -1;

  struct mmix_trap_diagnostic diagnostic = {
    .event_class = event_class,
    .cause = cause,
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
    .intc_pending = intc_pending,
    .intc_enabled = intc_enabled,
    .intc_claim = claim,
    .timer_pending = timer_pending,
  };

  mmix_early_print_trap(&diagnostic);
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
trap_external(struct mmix_trap_state *state)
{
  uint32 irq = 0;
  int pending;
  int status;

  if (!mmix_rq_intc_pending(state->rq, state->restore_rk))
    trap_stop(MMIX_TRAP_EXTERNAL, "masked request", state, irq);
  if (state->rxx != MMIX_DYNAMIC_TRAP_RESUME_NEXT)
    trap_stop(MMIX_TRAP_EXTERNAL, "unsupported resume", state, irq);

  status = mmix_intc_claim(&irq);
  if (status == MMIX_INTC_NO_IRQ)
    trap_stop(MMIX_TRAP_EXTERNAL, "zero claim", state, irq);
  if (status != MMIX_INTC_OK)
    trap_stop(MMIX_TRAP_EXTERNAL, "invalid claim", state, irq);
  if (irq != MMIX_TIMER_IRQ)
    trap_stop(MMIX_TRAP_EXTERNAL, "unexpected claim", state, irq);
  if (mmix_timer_pending(&pending) != MMIX_TIMER_OK || !pending)
    trap_stop(MMIX_TRAP_EXTERNAL, "timer not pending", state, irq);

  if (mmix_timer_arm_next() != MMIX_TIMER_OK)
    trap_stop(MMIX_TRAP_EXTERNAL, "timer rearm", state, irq);
  if (mmix_timer_acknowledge() != MMIX_TIMER_OK)
    trap_stop(MMIX_TRAP_EXTERNAL, "timer acknowledge", state, irq);
  if (mmix_intc_complete(irq) != MMIX_INTC_OK)
    trap_stop(MMIX_TRAP_EXTERNAL, "controller complete", state, irq);
  if (mmix_timer_record_tick() != MMIX_TIMER_OK)
    trap_stop(MMIX_TRAP_EXTERNAL, "tick overflow", state, irq);

  // GET/PUT rQ is a request handoff. Do not restore the serviced controller
  // bit or RESUME would immediately deliver the same dynamic trap again.
  state->rq &= ~MMIX_RQ_INTC;
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
    return;
  }
  trap_stop(event, "unknown", state, 0);
}
