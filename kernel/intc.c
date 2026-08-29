#include "boot.h"
#include "cpu.h"
#include "intc.h"

enum {
  MMIX_INTC_REGISTER_SIZE = 4,
  MMIX_INTC_PENDING_OFFSET = 0x0000,
  MMIX_INTC_CONTEXT_BASE = 0x1000,
  MMIX_INTC_CONTEXT_STRIDE = 0x100,
  MMIX_INTC_CONTEXT_ENABLE_OFFSET = 0x00,
  MMIX_INTC_CONTEXT_CLAIM_OFFSET = 0x04,
  MMIX_INTC_CONTEXT_COMPLETE_OFFSET = 0x08,
  MMIX_INTC_MAX_IRQ_COUNT = 32,
};

static int
intc_platform_valid(void)
{
  const struct mmix_bootinfo *info = &mmix_boot.info;

  return mmix_boot.bootinfo_status == MMIX_BOOTINFO_OK &&
         (info->intc_base & (MMIX_INTC_REGISTER_SIZE - 1)) == 0 &&
         info->boot_cpu_id == BOOT_CPU_ID && info->cpu_count > 0 &&
         info->cpu_count <= MMIX_MAX_CPUS &&
         info->intc_irq_count > 1 &&
         info->intc_irq_count <= MMIX_INTC_MAX_IRQ_COUNT;
}

static int
intc_current_valid(void)
{
  int id = cpuid();

  return intc_platform_valid() && id >= 0 &&
         (uint64)id < mmix_boot.info.cpu_count;
}

static int
intc_irq_valid(uint32 irq)
{
  return intc_platform_valid() && irq != 0 &&
         irq < mmix_boot.info.intc_irq_count;
}

static volatile uint32 *
intc_register(uint64 offset)
{
  return (volatile uint32 *)(mmix_boot.info.intc_base + offset);
}

static uint64
intc_context_register(uint64 offset)
{
  return MMIX_INTC_CONTEXT_BASE +
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

int
intc_validate(void)
{
  return intc_platform_valid() ? MMIX_INTC_OK : MMIX_INTC_BAD_PLATFORM;
}

int
intc_init(void)
{
  uint64 enable;

  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;

  enable = intc_context_register(MMIX_INTC_CONTEXT_ENABLE_OFFSET);
  intc_write(enable, 0);
  if (intc_read(enable) != 0)
    return MMIX_INTC_BAD_STATE;
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
intc_claim(uint32 *irq)
{
  uint32 claimed;

  if (irq == 0)
    return MMIX_INTC_BAD_ARGUMENT;
  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;

  claimed = intc_read(intc_context_register(MMIX_INTC_CONTEXT_CLAIM_OFFSET));
  *irq = claimed;
  if (claimed == 0)
    return MMIX_INTC_NO_IRQ;
  if (!intc_irq_valid(claimed))
    return MMIX_INTC_BAD_IRQ;
  return MMIX_INTC_OK;
}

int
intc_complete(uint32 irq)
{
  if (!intc_current_valid())
    return MMIX_INTC_BAD_PLATFORM;
  if (!intc_irq_valid(irq))
    return MMIX_INTC_BAD_IRQ;

  intc_write(intc_context_register(MMIX_INTC_CONTEXT_COMPLETE_OFFSET), irq);
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
