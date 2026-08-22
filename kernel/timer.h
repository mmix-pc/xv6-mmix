#ifndef XV6_MMIX_TIMER_H
#define XV6_MMIX_TIMER_H

enum {
  MMIX_TIMER_IRQ = 16,
};

enum mmix_timer_status {
  MMIX_TIMER_OK = 0,
  MMIX_TIMER_BAD_ARGUMENT = -1,
  MMIX_TIMER_BAD_PLATFORM = -2,
  MMIX_TIMER_BAD_STATE = -3,
  MMIX_TIMER_BAD_DEADLINE = -4,
};

int timer_init(void);
int timer_pending(int *pending);
int timer_disable(void);
int timer_acknowledge(void);
int timer_arm_next(void);
int timer_record_tick(void);
uint64 timer_ticks(void);

#endif
