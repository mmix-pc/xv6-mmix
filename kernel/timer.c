#include "boot.h"
#include "cpu.h"
#include "platform.h"
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

static uint64 tick_count[MMIX_MAX_CPUS];
static struct platform_timer_config timer_config;
static uint32 timer_interrupts[MMIX_MAX_CPUS];
static uint32 timer_cpu_count;
static uint64 timer_configured;

static int
timer_configure(void)
{
  struct platform_timer_config config;
  uint32 cpu_count;
  uint32 interrupts[MMIX_MAX_CPUS];

  if (__atomic_load_n(&timer_configured, __ATOMIC_ACQUIRE) != 0)
    return 1;
  if (cpuid() != BOOT_CPU_ID ||
      platform_timer_config(&config) != PLATFORM_OK)
    return 0;
  cpu_count = platform_cpu_count();
  if ((config.physical_global.physical_base &
       (MMIX_TIMER_REGISTER_SIZE - 1)) != 0 ||
      config.physical_contexts.physical_base !=
        config.physical_global.physical_base + MMIX_TIMER_CONTEXT_BASE ||
      config.context_stride != MMIX_TIMER_CONTEXT_STRIDE ||
      config.context_count != cpu_count || cpu_count == 0 ||
      cpu_count > MMIX_MAX_CPUS ||
      config.context_count > TIMER_IRQ_COUNT_MAX)
    return 0;
  for (uint32 id = 0; id < config.context_count; id++) {
    if (platform_timer_interrupt(id, &interrupts[id]) != PLATFORM_OK ||
        interrupts[id] != MMIX_TIMER_IRQ + id)
      return 0;
  }

  timer_config = config;
  timer_cpu_count = cpu_count;
  for (uint32 id = 0; id < cpu_count; id++)
    timer_interrupts[id] = interrupts[id];
  __atomic_store_n(&timer_configured, 1, __ATOMIC_RELEASE);
  return 1;
}

static int
timer_config_valid(void)
{
  return __atomic_load_n(&timer_configured, __ATOMIC_ACQUIRE) != 0;
}

static int
timer_current_valid(void)
{
  int id = cpuid();

  return timer_config_valid() && id >= 0 && (uint32)id < timer_cpu_count;
}

static volatile uint64 *
timer_register(uint64 offset)
{
  return (volatile uint64 *)(timer_config.physical_global.physical_base +
                             offset);
}

static uint64
timer_context_register(uint64 offset)
{
  return timer_config.physical_contexts.physical_base -
           timer_config.physical_global.physical_base +
         (uint64)cpuid() * timer_config.context_stride + offset;
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
timer_validate(void)
{
  return timer_configure() ? MMIX_TIMER_OK : MMIX_TIMER_BAD_PLATFORM;
}

int
timer_irq(uint32 *irq)
{
  if (irq == 0)
    return MMIX_TIMER_BAD_ARGUMENT;
  if (!timer_current_valid())
    return MMIX_TIMER_BAD_PLATFORM;
  *irq = timer_interrupts[cpuid()];
  return MMIX_TIMER_OK;
}

int
timer_init(void)
{
  uint64 compare;
  uint64 control;
  uint64 status;

  if (!timer_configure() || !timer_current_valid())
    return MMIX_TIMER_BAD_PLATFORM;

  tick_count[cpuid()] = 0;
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
  if (!timer_current_valid())
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

  if (!timer_current_valid())
    return MMIX_TIMER_BAD_PLATFORM;

  control = timer_context_register(MMIX_TIMER_CONTEXT_CONTROL_OFFSET);
  timer_write(control, 0);
  if (timer_read(control) != 0)
    return MMIX_TIMER_BAD_STATE;
  return MMIX_TIMER_OK;
}

// An expired timer is level-triggered. Disable it before acknowledging stale
// status, then rearm it before completing its interrupt-controller claim.
int
timer_acknowledge(void)
{
  uint64 status;

  if (!timer_current_valid())
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

  if (!timer_current_valid())
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
  int id = cpuid();

  if (!timer_current_valid())
    return MMIX_TIMER_BAD_PLATFORM;
  if (tick_count[id] == ~0ULL)
    return MMIX_TIMER_BAD_STATE;
  tick_count[id]++;
  return MMIX_TIMER_OK;
}

uint64
timer_ticks(void)
{
  return tick_count[cpuid()];
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
