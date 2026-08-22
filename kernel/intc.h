#ifndef XV6_MMIX_INTC_H
#define XV6_MMIX_INTC_H

enum mmix_intc_status {
  MMIX_INTC_OK = 0,
  MMIX_INTC_NO_IRQ = 1,
  MMIX_INTC_BAD_ARGUMENT = -1,
  MMIX_INTC_BAD_PLATFORM = -2,
  MMIX_INTC_BAD_IRQ = -3,
  MMIX_INTC_BAD_STATE = -4,
};

int intc_init(void);
int intc_pending(uint32 *pending);
int intc_enabled(uint32 *enabled);
int intc_set_enabled(uint32 irq, int enabled);
int intc_claim(uint32 *irq);
int intc_complete(uint32 irq);

#endif
