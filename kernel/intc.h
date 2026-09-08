#ifndef XV6_MMIX_INTC_H
#define XV6_MMIX_INTC_H

#define INTC_SOURCE_COUNT 8192
#define INTC_WORD_BITS 64
#define INTC_WORD_COUNT (INTC_SOURCE_COUNT / INTC_WORD_BITS)

enum mmix_intc_status {
  MMIX_INTC_OK = 0,
  MMIX_INTC_NO_IRQ = 1,
  MMIX_INTC_BAD_ARGUMENT = -1,
  MMIX_INTC_BAD_PLATFORM = -2,
  MMIX_INTC_BAD_IRQ = -3,
  MMIX_INTC_BAD_STATE = -4,
  MMIX_INTC_BAD_OWNER = -5,
};

int intc_validate(void);
int intc_init(void);
int intc_publish_affinity(void);
// Indexed octas cover the full source namespace without a stack snapshot.
int intc_pending(uint32 word, uint64 *pending);
int intc_enabled(uint32 word, uint64 *enabled);
int intc_set_enabled(uint32 irq, int enabled);
int intc_shared_owner(uint32 irq, uint32 *owner);
int intc_runtime_mask(uint32 timer_irq, uint32 word, uint64 *mask);
int intc_enable_runtime(uint32 timer_irq);
int intc_claim(uint32 *irq);
int intc_complete(uint32 irq);

#endif
