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

int mmix_timer_init(void);
int mmix_timer_pending(int *pending);
int mmix_timer_disable(void);
int mmix_timer_acknowledge(void);
int mmix_timer_arm_next(void);

#endif
