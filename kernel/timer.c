#include "boot.h"
#include "timer.h"

enum {
  MMIX_TIMER_REGISTER_SIZE = 8,
  MMIX_TIMER_TIME_OFFSET = 0x0000,
  MMIX_TIMER_CONTEXT_BASE = 0x0100,
  MMIX_TIMER_CONTEXT_STRIDE = 0x40,
  MMIX_TIMER_CONTEXT_COMPARE_OFFSET = 0x00,
  MMIX_TIMER_CONTEXT_CONTROL_OFFSET = 0x08,
  MMIX_TIMER_CONTEXT_STATUS_OFFSET = 0x10,
  MMIX_TIMER_CONTROL_ENABLE = 1 << 0,
  MMIX_TIMER_CONTROL_IRQ_ENABLE = 1 << 1,
  MMIX_TIMER_STATUS_PENDING = 1 << 0,
};

#define MMIX_TIMER_UNITS_PER_SECOND 1000000000ULL
#define MMIX_TIMER_TICKS_PER_SECOND 10ULL
#define MMIX_TIMER_TICK_INTERVAL                                               \
  (MMIX_TIMER_UNITS_PER_SECOND / MMIX_TIMER_TICKS_PER_SECOND)
#define MMIX_TIMER_MAX_DEADLINE 0x7fffffffffffffffULL

static volatile uint64 tick_count;

static int
timer_platform_valid(void)
{
  const struct mmix_bootinfo *info = &mmix_boot.info;

  return mmix_boot.bootinfo_status == MMIX_BOOTINFO_OK &&
         (info->timer_base & (MMIX_TIMER_REGISTER_SIZE - 1)) == 0 &&
         info->boot_cpu_id == 0 && info->timer_irq_base == MMIX_TIMER_IRQ &&
         info->timer_irq_count == 1 &&
         info->timer_irq_base < info->intc_irq_count;
}

static volatile uint64 *
timer_register(uint64 offset)
{
  return (volatile uint64 *)(mmix_boot.info.timer_base + offset);
}

static uint64
timer_context_register(uint64 offset)
{
  return MMIX_TIMER_CONTEXT_BASE +
         mmix_boot.info.boot_cpu_id * MMIX_TIMER_CONTEXT_STRIDE + offset;
}

static uint64
timer_read(uint64 offset)
{
  return *timer_register(offset);
}

static void
timer_write(uint64 offset, uint64 value)
{
  *timer_register(offset) = value;
}

int
timer_init(void)
{
  uint64 compare;
  uint64 control;
  uint64 status;

  if (!timer_platform_valid())
    return MMIX_TIMER_BAD_PLATFORM;

  tick_count = 0;
  compare = timer_context_register(MMIX_TIMER_CONTEXT_COMPARE_OFFSET);
  control = timer_context_register(MMIX_TIMER_CONTEXT_CONTROL_OFFSET);
  status = timer_context_register(MMIX_TIMER_CONTEXT_STATUS_OFFSET);
  timer_write(control, 0);
  timer_write(compare, 0);
  timer_write(status, MMIX_TIMER_STATUS_PENDING);
  if (timer_read(compare) != 0 || timer_read(control) != 0 ||
      timer_read(status) != 0)
    return MMIX_TIMER_BAD_STATE;
  return MMIX_TIMER_OK;
}

int
timer_pending(int *pending)
{
  uint64 status;

  if (pending == 0)
    return MMIX_TIMER_BAD_ARGUMENT;
  if (!timer_platform_valid())
    return MMIX_TIMER_BAD_PLATFORM;

  status = timer_read(timer_context_register(MMIX_TIMER_CONTEXT_STATUS_OFFSET));
  if ((status & ~MMIX_TIMER_STATUS_PENDING) != 0)
    return MMIX_TIMER_BAD_STATE;
  *pending = (status & MMIX_TIMER_STATUS_PENDING) != 0;
  return MMIX_TIMER_OK;
}

int
timer_disable(void)
{
  uint64 control;

  if (!timer_platform_valid())
    return MMIX_TIMER_BAD_PLATFORM;

  control = timer_context_register(MMIX_TIMER_CONTEXT_CONTROL_OFFSET);
  timer_write(control, 0);
  if (timer_read(control) != 0)
    return MMIX_TIMER_BAD_STATE;
  return MMIX_TIMER_OK;
}

// An expired timer is level-triggered. Rearm it or disable it, acknowledge
// pending status, and only then complete its interrupt-controller claim.
int
timer_acknowledge(void)
{
  uint64 status;

  if (!timer_platform_valid())
    return MMIX_TIMER_BAD_PLATFORM;

  status = timer_context_register(MMIX_TIMER_CONTEXT_STATUS_OFFSET);
  timer_write(status, MMIX_TIMER_STATUS_PENDING);
  if (timer_read(status) != 0)
    return MMIX_TIMER_BAD_STATE;
  return MMIX_TIMER_OK;
}

int
timer_arm_next(void)
{
  uint64 compare;
  uint64 control;
  uint64 now;
  uint64 next;

  if (!timer_platform_valid())
    return MMIX_TIMER_BAD_PLATFORM;

  now = timer_read(MMIX_TIMER_TIME_OFFSET);
  if (now > MMIX_TIMER_MAX_DEADLINE - MMIX_TIMER_TICK_INTERVAL)
    return MMIX_TIMER_BAD_DEADLINE;
  next = now + MMIX_TIMER_TICK_INTERVAL;

  compare = timer_context_register(MMIX_TIMER_CONTEXT_COMPARE_OFFSET);
  control = timer_context_register(MMIX_TIMER_CONTEXT_CONTROL_OFFSET);
  timer_write(compare, next);
  timer_write(control,
              MMIX_TIMER_CONTROL_ENABLE | MMIX_TIMER_CONTROL_IRQ_ENABLE);
  if (timer_read(compare) != next ||
      timer_read(control) !=
        (MMIX_TIMER_CONTROL_ENABLE | MMIX_TIMER_CONTROL_IRQ_ENABLE))
    return MMIX_TIMER_BAD_STATE;
  return MMIX_TIMER_OK;
}

int
timer_record_tick(void)
{
  if (tick_count == ~0ULL)
    return MMIX_TIMER_BAD_STATE;
  tick_count++;
  return MMIX_TIMER_OK;
}

uint64
timer_ticks(void)
{
  return tick_count;
}

_Static_assert((MMIX_TIMER_UNITS_PER_SECOND % MMIX_TIMER_TICKS_PER_SECOND) == 0,
               "xv6 tick interval must be exact in MMIX timer units");
_Static_assert((MMIX_TIMER_TIME_OFFSET & (MMIX_TIMER_REGISTER_SIZE - 1)) == 0,
               "MMIX timer time register must be octa-aligned");
_Static_assert((MMIX_TIMER_CONTEXT_BASE & (MMIX_TIMER_REGISTER_SIZE - 1)) ==
                   0 &&
                 (MMIX_TIMER_CONTEXT_STRIDE & (MMIX_TIMER_REGISTER_SIZE - 1)) ==
                   0,
               "MMIX timer contexts must be octa-aligned");
_Static_assert(
  (MMIX_TIMER_CONTEXT_COMPARE_OFFSET & (MMIX_TIMER_REGISTER_SIZE - 1)) == 0 &&
    (MMIX_TIMER_CONTEXT_CONTROL_OFFSET & (MMIX_TIMER_REGISTER_SIZE - 1)) == 0 &&
    (MMIX_TIMER_CONTEXT_STATUS_OFFSET & (MMIX_TIMER_REGISTER_SIZE - 1)) == 0,
  "MMIX timer context registers must be octa-aligned");
