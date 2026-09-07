#include "boot.h"
#include "cpu.h"
#include "intc.h"
#include "platform.h"

// FIXME: Remove after this driver adopts the platform query interface.
extern struct platform mmix_platform;

enum {
  MMIX_INTC_REGISTER_SIZE = 4,
  MMIX_INTC_PENDING_OFFSET = 0x0000,
  MMIX_INTC_CONTEXT_BASE = 0x1000,
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

static int
intc_platform_valid(void)
{
  const struct platform_interrupt_controller *intc =
    &mmix_platform.devices.interrupt_controller;

  return (intc->global.start & (MMIX_INTC_REGISTER_SIZE - 1)) == 0 &&
         intc->contexts.start == intc->global.start + MMIX_INTC_CONTEXT_BASE &&
         intc->context_stride == MMIX_INTC_CONTEXT_STRIDE &&
         mmix_platform.topology.count > 0 &&
         mmix_platform.topology.count <= MMIX_MAX_CPUS &&
         intc->context_count == mmix_platform.topology.count &&
         intc->source_count > 1 &&
         intc->source_count <= MMIX_INTC_MAX_IRQ_COUNT;
}

static int
intc_current_valid(void)
{
  int id = cpuid();

  return intc_platform_valid() && id >= 0 &&
         (uint64)id < mmix_platform.topology.count;
}

static int
intc_irq_valid(uint32 irq)
{
  return intc_platform_valid() && irq != 0 &&
         irq < mmix_platform.devices.interrupt_controller.source_count;
}

static volatile uint32 *
intc_register(uint64 offset)
{
  return (volatile uint32 *)(
    mmix_platform.devices.interrupt_controller.global.start + offset);
}

static uint64
intc_context_register(uint64 offset)
{
  return mmix_platform.devices.interrupt_controller.contexts.start -
           mmix_platform.devices.interrupt_controller.global.start +
         (uint64)cpuid() * MMIX_INTC_CONTEXT_STRIDE + offset;
}

static uint32
intc_read(uint64 offset)
{
  return *intc_register(offset);
}

static void
intc_write(uint64 offset, uint32 value)
{
  *intc_register(offset) = value;
}

static int
intc_affinity_valid(void)
{
  uint64 cpu_count = mmix_platform.topology.count;
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
  if (irq == mmix_platform.devices.timer.interrupts[id])
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
  return intc_platform_valid() ? MMIX_INTC_OK : MMIX_INTC_BAD_PLATFORM;
}

int
intc_init(void)
{
  int id = cpuid();
  uint64 enable;

  if (!intc_current_valid())
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
                   mmix_platform.topology.count > 1 ? 1 : BOOT_CPU_ID,
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

  *pending = intc_read(MMIX_INTC_PENDING_OFFSET);
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
  if (!intc_platform_valid() || !intc_affinity_valid())
    return MMIX_INTC_BAD_PLATFORM;
  if (irq == UART0_IRQ)
    selected = __atomic_load_n(&intc_affinity.uart_owner, __ATOMIC_RELAXED);
  else if (irq == VIRTIO0_IRQ)
    selected = __atomic_load_n(&intc_affinity.virtio_owner,
                               __ATOMIC_RELAXED);
  else
    return MMIX_INTC_BAD_IRQ;
  if (selected >= mmix_platform.topology.count)
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
  expected_timer = mmix_platform.devices.timer.interrupts[id];
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
_Static_assert((MMIX_INTC_CONTEXT_BASE & (MMIX_INTC_REGISTER_SIZE - 1)) == 0 &&
                 (MMIX_INTC_CONTEXT_STRIDE & (MMIX_INTC_REGISTER_SIZE - 1)) ==
                   0,
               "MMIX INTC contexts must be tetra-aligned");
_Static_assert(
  (MMIX_INTC_CONTEXT_ENABLE_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0 &&
    (MMIX_INTC_CONTEXT_CLAIM_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0 &&
    (MMIX_INTC_CONTEXT_COMPLETE_OFFSET & (MMIX_INTC_REGISTER_SIZE - 1)) == 0,
  "MMIX INTC context registers must be tetra-aligned");
_Static_assert(__alignof__(struct intc_affinity_state) == sizeof(uint64),
               "MMIX INTC affinity must be octa-aligned");
