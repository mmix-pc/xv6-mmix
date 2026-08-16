#ifndef XV6_MMIX_EARLY_PRINT_H
#define XV6_MMIX_EARLY_PRINT_H

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
};

void mmix_early_print_boot(const struct mmix_boot_state *boot);
void mmix_early_print_paging(uint64 rv);
void mmix_early_print_trap(const struct mmix_trap_diagnostic *diagnostic);
void panic(char *message) __attribute__((noreturn));

#endif
