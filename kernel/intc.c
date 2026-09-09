#include "boot.h"
#include "cpu.h"
#include "intc.h"
#include "mmix.h"
#include "defs.h"
#include "platform.h"
#include "timer.h"

enum {
  MMIX_INTC_REGISTER_SIZE = 8,
  MMIX_INTC_SOURCE_COUNT_OFFSET = 0x00,
  MMIX_INTC_CONTEXT_COUNT_OFFSET = 0x08,
  MMIX_INTC_PENDING_OFFSET = 0x1000,
  MMIX_INTC_CONTEXT_STRIDE = 0x10000,
  MMIX_INTC_CONTEXT_ENABLE_OFFSET = 0x00,
  MMIX_INTC_CONTEXT_CLAIM_OFFSET = 0x800,
  MMIX_INTC_CONTEXT_COMPLETE_OFFSET = 0x808,
  MMIX_INTC_AFFINITY_GENERATION = 1,
};

struct intc_affinity_state {
  uint64 generation;
  uint64 uart_owner;
  uint64 virtio_owner;
};

static struct intc_affinity_state intc_affinity;
static uint64 intc_active_claim[MMIX_MAX_CPUS];
static struct platform_intc_config intc_config;
static uint32 intc_cpu_count;
static uint32 intc_timer_interrupts[MMIX_MAX_CPUS];
static uint64 intc_configured;
static uint32 intc_uart_irq;
static uint32 intc_disk_irq;

static int
intc_configure(void)
{
  struct platform_intc_config config;
  uint32 cpu_count;
  uint32 timer_interrupts[MMIX_MAX_CPUS];
  struct platform_uart_config uart;
  uint64 address;

  if (__atomic_load_n(&intc_configured, __ATOMIC_ACQUIRE) != 0)
    return 1;
  if (cpuid() != BOOT_CPU_ID ||
      platform_intc_config(&config) != PLATFORM_OK)
    return 0;
  cpu_count = platform_cpu_count();
  if ((config.physical_global.physical_base &
       (MMIX_INTC_REGISTER_SIZE - 1)) != 0 ||
      config.context_stride != MMIX_INTC_CONTEXT_STRIDE || cpu_count == 0 ||
      cpu_count > MMIX_MAX_CPUS || config.context_count != cpu_count ||
      config.source_count != INTC_SOURCE_COUNT ||
      platform_uart_config(&uart) != PLATFORM_OK ||
      uart.interrupt != UART0_IRQ)
    return 0;
  if (mmix_mmio_address(config.physical_global.physical_base,
                         config.physical_global.size,
                         MMIX_INTC_SOURCE_COUNT_OFFSET, 8, &address) < 0 ||
      *(volatile uint64 *)address != config.source_count ||
      mmix_mmio_address(config.physical_global.physical_base,
                         config.physical_global.size,
                         MMIX_INTC_CONTEXT_COUNT_OFFSET, 8, &address) < 0 ||
      *(volatile uint64 *)address != cpu_count)
    return 0;
  for (uint32 id = 0; id < cpu_count; id++)
    if (platform_timer_interrupt(id, &timer_interrupts[id]) != PLATFORM_OK ||
        timer_interrupts[id] != MMIX_TIMER_IRQ + id ||
        timer_interrupts[id] >= config.source_count)
      return 0;

  intc_config = config;
  intc_cpu_count = cpu_count;
  intc_uart_irq = uart.interrupt;
  for (uint32 id = 0; id < cpu_count; id++)
    intc_timer_interrupts[id] = timer_interrupts[id];
  __atomic_store_n(&intc_configured, 1, __ATOMIC_RELEASE);
  return 1;
}

static int
intc_config_valid(void)
{
  return __atomic_load_n(&intc_configured, __ATOMIC_ACQUIRE) != 0;
}

static int
intc_current_valid(void)
{
  int id = cpuid();

  return intc_config_valid() && id >= 0 && (uint32)id < intc_cpu_count;
}

static int
intc_irq_valid(uint32 irq)
{
  return intc_config_valid() && irq != 0 && irq < intc_config.source_count;
}

static uint64
intc_register(uint64 offset)
{
  uint64 address;

  if (mmix_mmio_address(intc_config.physical_global.physical_base,
                         intc_config.physical_global.size, offset,
                         MMIX_INTC_REGISTER_SIZE, &address) < 0)
    panic("intc register");
  return address;
}

static uint64
intc_context_register(uint64 offset)
{
  uint64 address;

  if (mmix_mmio_context_address(intc_config.physical_contexts.physical_base,
                                 intc_config.physical_contexts.size, cpuid(),
                                 intc_config.context_count,
                                 intc_config.context_stride, offset,
                                 MMIX_INTC_REGISTER_SIZE, &address) < 0)
    panic("intc context register");
  return address;
}

static uint64
intc_read(uint64 address)
{
  return *(volatile uint64 *)address;
}

static void
intc_write(uint64 address, uint64 value)
{
  *(volatile uint64 *)address = value;
}

static int
intc_affinity_valid(void)
{
  uint64 cpu_count = intc_cpu_count;
  uint64 uart_owner;
  uint64 virtio_owner;

  if (__atomic_load_n(&intc_affinity.generation, __ATOMIC_ACQUIRE) !=
      MMIX_INTC_AFFINITY_GENERATION)
    return 0;
  uart_owner = __atomic_load_n(&intc_affinity.uart_owner, __ATOMIC_RELAXED);
  virtio_owner = __atomic_load_n(&intc_affinity.virtio_owner,
                                 __ATOMIC_RELAXED);
  return uart_owner == BOOT_CPU_ID &&
         virtio_owner == (cpu_count > 1 ? 1 : BOOT_CPU_ID) &&
         uart_owner < cpu_count && virtio_owner < cpu_count;
}

static int
intc_current_owns(uint32 irq)
{
  uint32 id = (uint32)cpuid();

  if (!intc_current_valid() || !intc_affinity_valid())
    return 0;
  if (irq == intc_timer_interrupts[id])
    return 1;
  if (irq == intc_uart_irq)
    return __atomic_load_n(&intc_affinity.uart_owner,
                           __ATOMIC_RELAXED) == id;
  if (irq != 0 && irq == intc_virtio_irq())
    return __atomic_load_n(&intc_affinity.virtio_owner,
                           __ATOMIC_RELAXED) == id;
  return 0;
}

int
intc_validate(void)
{
  return intc_configure() ? MMIX_INTC_OK : MMIX_INTC_BAD_PLATFORM;
}

int
intc_init(void)
{
  int id = cpuid();
  uint64 enable;

  if (!intc_configure() || !intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;

  if (intc_active_claim[id] != 0)
    return MMIX_INTC_BAD_STATE;
  for (uint32 word = 0; word < INTC_WORD_COUNT; word++) {
    enable = intc_context_register(MMIX_INTC_CONTEXT_ENABLE_OFFSET +
                                    word * MMIX_INTC_REGISTER_SIZE);
    intc_write(enable, 0);
    if (intc_read(enable) != 0)
      return MMIX_INTC_BAD_STATE;
  }
  intc_active_claim[id] = 0;

  if (id != BOOT_CPU_ID && !intc_affinity_valid())
    return MMIX_INTC_BAD_STATE;
  return MMIX_INTC_OK;
}

int
intc_publish_affinity(void)
{
  uint64 enabled;

  if (cpuid() != BOOT_CPU_ID || !intc_current_valid() ||
      __atomic_load_n(&intc_affinity.generation, __ATOMIC_RELAXED) != 0)
    return MMIX_INTC_BAD_STATE;
  for (uint32 word = 0; word < INTC_WORD_COUNT; word++)
    if (intc_enabled(word, &enabled) != MMIX_INTC_OK || enabled != 0)
      return MMIX_INTC_BAD_STATE;
  __atomic_store_n(&intc_affinity.uart_owner, BOOT_CPU_ID,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&intc_affinity.virtio_owner,
                   intc_cpu_count > 1 ? 1 : BOOT_CPU_ID,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&intc_affinity.generation,
                   MMIX_INTC_AFFINITY_GENERATION, __ATOMIC_RELEASE);
  return MMIX_INTC_OK;
}

int
intc_bind_virtio_irq(uint32 irq)
{
  struct platform_virtio_config config;
  uint32 count = platform_virtio_count();

  if (!intc_current_valid() || cpuid() != BOOT_CPU_ID || intr_get() ||
      __atomic_load_n(&intc_affinity.generation, __ATOMIC_ACQUIRE) != 0 ||
      intc_virtio_irq() != 0)
    return MMIX_INTC_BAD_STATE;
  if (!intc_irq_valid(irq) || count == 0 || count > PLATFORM_VIRTIO_SLOTS)
    return MMIX_INTC_BAD_IRQ;
  for (uint32 i = 0; i < count; i++) {
    if (platform_virtio_config(i, &config) != PLATFORM_OK)
      return MMIX_INTC_BAD_PLATFORM;
    if (config.interrupt == irq) {
      __atomic_store_n(&intc_disk_irq, irq, __ATOMIC_RELEASE);
      return MMIX_INTC_OK;
    }
  }
  return MMIX_INTC_BAD_IRQ;
}

uint32
intc_virtio_irq(void)
{
  return __atomic_load_n(&intc_disk_irq, __ATOMIC_ACQUIRE);
}

int
intc_pending(uint32 word, uint64 *pending)
{
  if (pending == 0 || word >= INTC_WORD_COUNT)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;

  *pending = intc_read(intc_register(MMIX_INTC_PENDING_OFFSET +
                                     word * MMIX_INTC_REGISTER_SIZE));
  return MMIX_INTC_OK;
}

int
intc_enabled(uint32 word, uint64 *enabled)
{
  if (enabled == 0 || word >= INTC_WORD_COUNT)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;

  *enabled = intc_read(intc_context_register(MMIX_INTC_CONTEXT_ENABLE_OFFSET +
                                             word * MMIX_INTC_REGISTER_SIZE));
  return MMIX_INTC_OK;
}

int
intc_set_enabled(uint32 irq, int enabled)
{
  uint64 offset;
  uint64 mask;
  uint64 value;

  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;
  if (!intc_irq_valid(irq))
    return MMIX_INTC_BAD_IRQ;
  if (enabled != 0 && enabled != 1)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_owns(irq))
    return MMIX_INTC_BAD_OWNER;
  if (intc_active_claim[cpuid()] == irq)
    return MMIX_INTC_BAD_STATE;

  offset = intc_context_register(MMIX_INTC_CONTEXT_ENABLE_OFFSET +
                                  (irq / INTC_WORD_BITS) *
                                    MMIX_INTC_REGISTER_SIZE);
  mask = 1ULL << (irq % INTC_WORD_BITS);
  value = intc_read(offset);
  value = enabled ? value | mask : value & ~mask;
  intc_write(offset, value);
  if (intc_read(offset) != value)
    return MMIX_INTC_BAD_STATE;
  return MMIX_INTC_OK;
}

int
intc_shared_owner(uint32 irq, uint32 *owner)
{
  uint64 selected;

  if (owner == 0)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_config_valid() || !intc_affinity_valid())
    return MMIX_INTC_BAD_PLATFORM;
  if (irq == intc_uart_irq)
    selected = __atomic_load_n(&intc_affinity.uart_owner, __ATOMIC_RELAXED);
  else if (irq != 0 && irq == intc_virtio_irq())
    selected = __atomic_load_n(&intc_affinity.virtio_owner,
                               __ATOMIC_RELAXED);
  else
    return MMIX_INTC_BAD_IRQ;
  if (selected >= intc_cpu_count)
    return MMIX_INTC_BAD_STATE;
  *owner = (uint32)selected;
  return MMIX_INTC_OK;
}

int
intc_runtime_mask(uint32 timer_irq, uint32 word, uint64 *mask)
{
  uint32 expected_timer;
  int id = cpuid();

  if (mask == 0 || word >= INTC_WORD_COUNT)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_valid() || !intc_affinity_valid())
    return MMIX_INTC_BAD_PLATFORM;
  expected_timer = intc_timer_interrupts[id];
  if (timer_irq != expected_timer || !intc_irq_valid(timer_irq))
    return MMIX_INTC_BAD_IRQ;

  // Shared devices opt in explicitly only after their drivers are ready.
  *mask = word == timer_irq / INTC_WORD_BITS
            ? 1ULL << (timer_irq % INTC_WORD_BITS) : 0;
  return MMIX_INTC_OK;
}

int
intc_enable_runtime(uint32 timer_irq)
{
  uint64 offset;
  uint64 current;
  uint64 expected;

  if (intc_runtime_mask(timer_irq, 0, &expected) != MMIX_INTC_OK)
    return MMIX_INTC_BAD_STATE;
  if (intc_active_claim[cpuid()] != 0)
    return MMIX_INTC_BAD_STATE;
  for (uint32 word = 0; word < INTC_WORD_COUNT; word++)
    if (intc_enabled(word, &current) != MMIX_INTC_OK || current != 0)
      return MMIX_INTC_BAD_STATE;
  for (uint32 word = 0; word < INTC_WORD_COUNT; word++) {
    if (intc_runtime_mask(timer_irq, word, &expected) != MMIX_INTC_OK)
      return MMIX_INTC_BAD_STATE;
    offset = intc_context_register(MMIX_INTC_CONTEXT_ENABLE_OFFSET +
                                    word * MMIX_INTC_REGISTER_SIZE);
    intc_write(offset, expected);
    if (intc_read(offset) != expected)
      return MMIX_INTC_BAD_STATE;
  }
  return MMIX_INTC_OK;
}

int
intc_claim(uint32 *irq)
{
  uint64 claimed;
  int id = cpuid();

  if (irq == 0)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;
  if (intc_active_claim[id] != 0)
    return MMIX_INTC_BAD_STATE;

  claimed = intc_read(intc_context_register(MMIX_INTC_CONTEXT_CLAIM_OFFSET));
  *irq = 0;
  if (claimed == 0)
    return MMIX_INTC_NO_IRQ;
  // Retain even a rejected hardware claim until fatal handling stops this CPU.
  intc_active_claim[id] = claimed;
  if (claimed >= INTC_SOURCE_COUNT)
    return MMIX_INTC_BAD_IRQ;
  *irq = (uint32)claimed;
  if (!intc_current_owns(claimed))
    return MMIX_INTC_BAD_OWNER;
  return MMIX_INTC_OK;
}

int
intc_complete(uint32 irq)
{
  int id = cpuid();

  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;
  if (!intc_irq_valid(irq))
    return MMIX_INTC_BAD_IRQ;
  if (!intc_current_owns(irq))
    return MMIX_INTC_BAD_OWNER;
  if (intc_active_claim[id] != irq)
    return MMIX_INTC_BAD_STATE;

  intc_write(intc_context_register(MMIX_INTC_CONTEXT_COMPLETE_OFFSET), irq);
  intc_active_claim[id] = 0;
  return MMIX_INTC_OK;
}

_Static_assert((MMIX_INTC_PENDING_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0,
               "MMIX INTC pending register must be octa-aligned");
_Static_assert((MMIX_INTC_CONTEXT_STRIDE & (MMIX_INTC_REGISTER_SIZE - 1)) == 0,
               "MMIX INTC contexts must be octa-aligned");
_Static_assert(
  (MMIX_INTC_CONTEXT_ENABLE_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0 &&
    (MMIX_INTC_CONTEXT_CLAIM_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0 &&
    (MMIX_INTC_CONTEXT_COMPLETE_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0,
  "MMIX INTC context registers must be octa-aligned");
_Static_assert(__alignof__(struct intc_affinity_state) == sizeof(uint64),
               "MMIX INTC affinity must be octa-aligned");
