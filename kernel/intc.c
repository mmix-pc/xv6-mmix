#include "boot.h"
#include "cpu.h"
#include "intc.h"
#include "mmix.h"
#include "defs.h"
#include "platform.h"
#include "timer.h"

enum {
  MMIX_INTC_REGISTER_SIZE = 4,
  MMIX_INTC_PENDING_OFFSET = 0x0000,
  MMIX_INTC_CONTEXT_STRIDE = 0x100,
  MMIX_INTC_CONTEXT_ENABLE_OFFSET = 0x00,
  MMIX_INTC_CONTEXT_CLAIM_OFFSET = 0x04,
  MMIX_INTC_CONTEXT_COMPLETE_OFFSET = 0x08,
  MMIX_INTC_MAX_IRQ_COUNT = 32,
  MMIX_INTC_AFFINITY_GENERATION = 1,
};

struct intc_affinity_state {
  uint64 generation;
  uint64 uart_owner;
  uint64 virtio_owner;
};

static struct intc_affinity_state intc_affinity;
static uint32 intc_active_claim[MMIX_MAX_CPUS];
static struct platform_intc_config intc_config;
static uint32 intc_cpu_count;
static uint32 intc_timer_interrupts[MMIX_MAX_CPUS];
static uint64 intc_configured;

static int
intc_configure(void)
{
  struct platform_intc_config config;
  uint32 cpu_count;
  uint32 timer_interrupts[MMIX_MAX_CPUS];

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
      config.source_count <= 1 ||
      config.source_count > MMIX_INTC_MAX_IRQ_COUNT)
    return 0;
  for (uint32 id = 0; id < cpu_count; id++)
    if (platform_timer_interrupt(id, &timer_interrupts[id]) != PLATFORM_OK ||
        timer_interrupts[id] != MMIX_TIMER_IRQ + id ||
        timer_interrupts[id] >= config.source_count)
      return 0;

  intc_config = config;
  intc_cpu_count = cpu_count;
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

static uint32
intc_read(uint64 address)
{
  return *(volatile uint32 *)address;
}

static void
intc_write(uint64 address, uint32 value)
{
  *(volatile uint32 *)address = value;
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
  if (irq == UART0_IRQ)
    return __atomic_load_n(&intc_affinity.uart_owner,
                           __ATOMIC_RELAXED) == id;
  if (irq == VIRTIO0_IRQ)
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

  enable = intc_context_register(MMIX_INTC_CONTEXT_ENABLE_OFFSET);
  intc_write(enable, 0);
  if (intc_read(enable) != 0)
    return MMIX_INTC_BAD_STATE;
  intc_active_claim[id] = 0;

  if (id != BOOT_CPU_ID && !intc_affinity_valid())
    return MMIX_INTC_BAD_STATE;
  return MMIX_INTC_OK;
}

int
intc_publish_affinity(void)
{
  uint32 enabled;

  if (cpuid() != BOOT_CPU_ID || !intc_current_valid() ||
      intc_enabled(&enabled) != MMIX_INTC_OK || enabled != 0 ||
      __atomic_load_n(&intc_affinity.generation, __ATOMIC_RELAXED) != 0)
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
intc_pending(uint32 *pending)
{
  if (pending == 0)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;

  *pending = intc_read(intc_register(MMIX_INTC_PENDING_OFFSET));
  return MMIX_INTC_OK;
}

int
intc_enabled(uint32 *enabled)
{
  if (enabled == 0)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;

  *enabled = intc_read(intc_context_register(MMIX_INTC_CONTEXT_ENABLE_OFFSET));
  return MMIX_INTC_OK;
}

int
intc_set_enabled(uint32 irq, int enabled)
{
  uint64 offset;
  uint32 mask;
  uint32 value;

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

  offset = intc_context_register(MMIX_INTC_CONTEXT_ENABLE_OFFSET);
  mask = 1U << irq;
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
  if (irq == UART0_IRQ)
    selected = __atomic_load_n(&intc_affinity.uart_owner, __ATOMIC_RELAXED);
  else if (irq == VIRTIO0_IRQ)
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
intc_runtime_mask(uint32 timer_irq, uint32 *mask)
{
  uint32 uart_owner;
  uint32 virtio_owner;
  uint32 expected_timer;
  int id = cpuid();

  if (mask == 0)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_valid() || !intc_affinity_valid())
    return MMIX_INTC_BAD_PLATFORM;
  expected_timer = intc_timer_interrupts[id];
  if (timer_irq != expected_timer || !intc_irq_valid(timer_irq) ||
      intc_shared_owner(UART0_IRQ, &uart_owner) != MMIX_INTC_OK ||
      intc_shared_owner(VIRTIO0_IRQ, &virtio_owner) != MMIX_INTC_OK)
    return MMIX_INTC_BAD_IRQ;

  *mask = 1U << timer_irq;
  if ((uint32)id == uart_owner)
    *mask |= 1U << UART0_IRQ;
  if ((uint32)id == virtio_owner)
    *mask |= 1U << VIRTIO0_IRQ;
  return MMIX_INTC_OK;
}

int
intc_enable_runtime(uint32 timer_irq)
{
  uint64 offset;
  uint32 current;
  uint32 expected;

  if (intc_runtime_mask(timer_irq, &expected) != MMIX_INTC_OK)
    return MMIX_INTC_BAD_STATE;
  if (intc_active_claim[cpuid()] != 0)
    return MMIX_INTC_BAD_STATE;
  offset = intc_context_register(MMIX_INTC_CONTEXT_ENABLE_OFFSET);
  current = intc_read(offset);
  if (current != 0)
    return MMIX_INTC_BAD_STATE;
  intc_write(offset, expected);
  if (intc_read(offset) != expected)
    return MMIX_INTC_BAD_STATE;
  return MMIX_INTC_OK;
}

int
intc_claim(uint32 *irq)
{
  uint32 claimed;
  int id = cpuid();

  if (irq == 0)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;
  if (intc_active_claim[id] != 0)
    return MMIX_INTC_BAD_STATE;

  claimed = intc_read(intc_context_register(MMIX_INTC_CONTEXT_CLAIM_OFFSET));
  *irq = claimed;
  if (claimed == 0)
    return MMIX_INTC_NO_IRQ;
  if (!intc_irq_valid(claimed))
    return MMIX_INTC_BAD_IRQ;
  intc_active_claim[id] = claimed;
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
  if (intc_active_claim[id] != irq)
    return MMIX_INTC_BAD_STATE;

  intc_write(intc_context_register(MMIX_INTC_CONTEXT_COMPLETE_OFFSET), irq);
  intc_active_claim[id] = 0;
  return MMIX_INTC_OK;
}

_Static_assert((MMIX_INTC_PENDING_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0,
               "MMIX INTC pending register must be tetra-aligned");
_Static_assert((MMIX_INTC_CONTEXT_STRIDE & (MMIX_INTC_REGISTER_SIZE - 1)) == 0,
               "MMIX INTC contexts must be tetra-aligned");
_Static_assert(
  (MMIX_INTC_CONTEXT_ENABLE_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0 &&
    (MMIX_INTC_CONTEXT_CLAIM_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0 &&
    (MMIX_INTC_CONTEXT_COMPLETE_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0,
  "MMIX INTC context registers must be tetra-aligned");
_Static_assert(__alignof__(struct intc_affinity_state) == sizeof(uint64),
               "MMIX INTC affinity must be octa-aligned");
