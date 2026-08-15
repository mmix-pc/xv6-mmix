#include "memlayout.h"
#include "mmix.h"
#include "early_print.h"

extern char kernel_text_end[];
extern char mmix_kernel_trap_entry[];

volatile uint64 mmix_trap_active;
uint64 mmix_trap_rk_shadow;
uint64 mmix_trap_vector;

void
trapinit(void)
{
  uint64 entry = (uint64)mmix_kernel_trap_entry;
  uint64 ro = mmix_ro_read();
  uint64 rs = mmix_rs_read();
  uint64 sp = mmix_sp_read();

  mmix_intr_off();
  mmix_trap_rk_shadow = 0;
  mmix_trap_active = 0;
  mmix_ra_write(mmix_ra_disable_trips(mmix_ra_read()));

  if (ro < REGISTER_STACK_BASE || ro >= REGISTER_STACK_LIMIT ||
      rs < REGISTER_STACK_BASE || rs >= REGISTER_STACK_LIMIT)
    panic("trap register stack");
  if (sp <= BOOT_STACK_BASE + MMIX_TRAP_STATE_SIZE || sp > BOOT_STACK_TOP)
    panic("trap software stack");
  if (mmix_trap_vector_make(entry, (uint64)kernel_text_end,
                            &mmix_trap_vector) < 0)
    panic("trap vector");

  // rT/rTT will execute the same bytes through the privileged physical alias.
  if (*(volatile uint *)entry != *(volatile uint *)mmix_trap_vector)
    panic("trap alias");
}

void
trapinithart(void)
{
  mmix_intr_off();
  if (mmix_trap_vector == 0 || mmix_trap_active != 0)
    panic("trap state");

  mmix_rt_write(mmix_trap_vector);
  mmix_rtt_write(mmix_trap_vector);
  if (mmix_rt_read() != mmix_trap_vector ||
      mmix_rtt_read() != mmix_trap_vector || mmix_rk_read() != 0)
    panic("trap install");
}

void
mmix_kernel_trap(enum mmix_trap_class event,
                 struct mmix_trap_state *state)
{
  (void)event;
  (void)state;
  // Controlled exception policy is added after the masked entry is stable.
  panic("kernel trap");
}
