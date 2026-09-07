#ifndef XV6_MMIX_DIAGNOSTIC_H
#define XV6_MMIX_DIAGNOSTIC_H

struct mmix_boot_state;
struct kalloc_stats;

struct mmix_trap_diagnostic {
  int from_user;
  const char *event_class;
  const char *cause;
  int pid;
  const char *process_name;
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
  int ipi_pending;
  uint64 ipi_received;
  uint64 ipi_acknowledged;
};

void diagnostic_boot(const struct mmix_boot_state *boot);
void diagnostic_startup_failure(uint64, int);
void diagnostic_platform_checkpoint(void);
void diagnostic_allocator(const struct kalloc_stats *stats);
void diagnostic_paging(uint64 rv);
void diagnostic_startup(uint64 cpu_count, uint64 online);
void diagnostic_trap(const struct mmix_trap_diagnostic *diagnostic);

#endif
