#ifndef XV6_MMIX_DIAGNOSTIC_H
#define XV6_MMIX_DIAGNOSTIC_H

struct mmix_boot_state;

struct mmix_trap_diagnostic {
  const char *event_class;
  const char *cause;
  uint64 rq;
  uint64 active_rk;
  uint64 restore_rk;
  uint64 rww;
  uint64 rxx;
  uint64 ryy;
  uint64 rzz;
  uint64 state;
  uint64 sp;
  uint64 fp;
  uint64 ro;
  uint64 rs;
  uint64 rl;
  uint32 intc_pending;
  uint32 intc_enabled;
  uint32 intc_claim;
  int timer_pending;
};

void diagnostic_boot(const struct mmix_boot_state *boot);
void diagnostic_paging(uint64 rv);
void diagnostic_trap(const struct mmix_trap_diagnostic *diagnostic);

#endif
