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

int mmix_intc_init(void);
int mmix_intc_pending(uint32 *pending);
int mmix_intc_enabled(uint32 *enabled);
int mmix_intc_set_enabled(uint32 irq, int enabled);
int mmix_intc_claim(uint32 *irq);
int mmix_intc_complete(uint32 irq);

#endif
