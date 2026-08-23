#include "types.h"
#include "memlayout.h"
#include "bootinfo.h"

_Static_assert(MMIX_BOOTINFO_MIN_SIZE == BOOTINFO_SIZE,
               "boot-info ABI and platform layout sizes must agree");

static __attribute__((always_inline)) inline int
range_contains(uint64 outer_base, uint64 outer_size, uint64 inner_base,
               uint64 inner_size)
{
  if (inner_base < outer_base || inner_size > outer_size)
    return 0;

  return inner_base - outer_base <= outer_size - inner_size;
}

static __attribute__((always_inline)) inline uint64
load_be_octa(const volatile uint8 *wire, enum mmix_bootinfo_field field)
{
  uint64 value = 0;
  uint64 offset = MMIX_BOOTINFO_FIELD_OFFSET(field);

  for (int i = 0; i < MMIX_BOOTINFO_OCTA_SIZE; i++)
    value = (value << 8) | wire[offset + i];

  return value;
}

static __attribute__((always_inline)) inline int
valid_memory_layout(const struct mmix_bootinfo *info)
{
  if (info->ram_base != LOW_RAM_BASE || info->ram_size < RAM_REQUIRED_SIZE ||
      !range_contains(info->ram_base, info->ram_size, LOW_RAM_BASE,
                      RAM_REQUIRED_SIZE) ||
      !range_contains(info->ram_base, info->ram_size, BOOTINFO_BASE,
                      BOOTINFO_SIZE))
    return 0;

  if (info->low_ram_base != LOW_RAM_BASE ||
      info->low_ram_size != LOW_RAM_SIZE ||
      !range_contains(info->ram_base, info->ram_size, info->low_ram_base,
                      info->low_ram_size))
    return 0;

  if (info->pool_logical_base != POOL_LOGICAL_BASE ||
      info->pool_phys_base != POOL_PHYS_BASE || info->pool_size != POOL_SIZE ||
      !range_contains(info->ram_base, info->ram_size, info->pool_phys_base,
                      info->pool_size))
    return 0;

  if (info->data_logical_base != DATA_LOGICAL_BASE ||
      info->data_phys_base != DATA_PHYS_BASE || info->data_size != DATA_SIZE ||
      !range_contains(info->ram_base, info->ram_size, info->data_phys_base,
                      info->data_size))
    return 0;

  if (info->stack_logical_base != STACK_LOGICAL_BASE ||
      info->stack_phys_base != STACK_PHYS_BASE ||
      info->stack_size != STACK_SIZE ||
      !range_contains(info->ram_base, info->ram_size, info->stack_phys_base,
                      info->stack_size))
    return 0;

  return 1;
}

static __attribute__((always_inline)) inline int
valid_devices(const struct mmix_bootinfo *info)
{
  return info->mmio_base == MMIO_BASE && info->uart_base == UART0_BASE &&
         info->uart_irq == UART0_IRQ && info->timer_base == TIMER_BASE &&
         info->timer_irq_base == TIMER_IRQ_BASE &&
         info->timer_irq_count == TIMER_IRQ_COUNT &&
         info->intc_base == INTC_BASE &&
         info->intc_irq_count == INTC_IRQ_COUNT &&
         info->virtio_mmio_base == VIRTIO0_BASE &&
         info->virtio_mmio_irq == VIRTIO0_IRQ &&
         info->virtio_mmio_count == VIRTIO_MMIO_COUNT;
}

int
bootinfo_decode(uint64 startup_cpu_id, uint64 bootinfo_pa,
                     struct mmix_bootinfo *decoded)
{
  const volatile uint8 *wire;
  struct mmix_bootinfo info;
  uint64 declared_size;

  if (decoded == 0)
    return MMIX_BOOTINFO_BAD_ARGUMENT;

  if ((bootinfo_pa & (MMIX_BOOTINFO_OCTA_SIZE - 1)) != 0 ||
      bootinfo_pa != BOOTINFO_BASE ||
      !range_contains(PLATFORM_RAM_BASE, PLATFORM_RAM_SIZE, bootinfo_pa,
                      3 * MMIX_BOOTINFO_OCTA_SIZE))
    return MMIX_BOOTINFO_BAD_ADDRESS;

  wire = (const volatile uint8 *)bootinfo_pa;
  if (load_be_octa(wire, MMIX_BOOTINFO_MAGIC_FIELD) != MMIX_BOOTINFO_MAGIC ||
      load_be_octa(wire, MMIX_BOOTINFO_VERSION_FIELD) !=
          MMIX_BOOTINFO_VERSION)
    return MMIX_BOOTINFO_BAD_HEADER;

  declared_size = load_be_octa(wire, MMIX_BOOTINFO_SIZE_FIELD);
  if (declared_size < MMIX_BOOTINFO_MIN_SIZE ||
      (declared_size & (MMIX_BOOTINFO_OCTA_SIZE - 1)) != 0 ||
      !range_contains(PLATFORM_RAM_BASE, PLATFORM_RAM_SIZE, bootinfo_pa,
                      declared_size) ||
      load_be_octa(wire, MMIX_BOOTINFO_FLAGS_FIELD) != 0)
    return MMIX_BOOTINFO_BAD_HEADER;

  info.cpu_count = load_be_octa(wire, MMIX_BOOTINFO_CPU_COUNT_FIELD);
  info.boot_cpu_id = load_be_octa(wire, MMIX_BOOTINFO_BOOT_CPU_ID_FIELD);
  if (info.cpu_count != BOOT_CPU_COUNT || info.boot_cpu_id != startup_cpu_id ||
      info.boot_cpu_id >= info.cpu_count || startup_cpu_id != BOOT_CPU_ID)
    return MMIX_BOOTINFO_BAD_CPU;

  info.ram_base = load_be_octa(wire, MMIX_BOOTINFO_RAM_BASE_FIELD);
  info.ram_size = load_be_octa(wire, MMIX_BOOTINFO_RAM_SIZE_FIELD);
  info.low_ram_base = load_be_octa(wire, MMIX_BOOTINFO_LOW_RAM_BASE_FIELD);
  info.low_ram_size = load_be_octa(wire, MMIX_BOOTINFO_LOW_RAM_SIZE_FIELD);
  info.pool_logical_base =
      load_be_octa(wire, MMIX_BOOTINFO_POOL_LOGICAL_BASE_FIELD);
  info.pool_phys_base =
      load_be_octa(wire, MMIX_BOOTINFO_POOL_PHYS_BASE_FIELD);
  info.pool_size = load_be_octa(wire, MMIX_BOOTINFO_POOL_SIZE_FIELD);
  info.data_logical_base =
      load_be_octa(wire, MMIX_BOOTINFO_DATA_LOGICAL_BASE_FIELD);
  info.data_phys_base =
      load_be_octa(wire, MMIX_BOOTINFO_DATA_PHYS_BASE_FIELD);
  info.data_size = load_be_octa(wire, MMIX_BOOTINFO_DATA_SIZE_FIELD);
  info.stack_logical_base =
      load_be_octa(wire, MMIX_BOOTINFO_STACK_LOGICAL_BASE_FIELD);
  info.stack_phys_base =
      load_be_octa(wire, MMIX_BOOTINFO_STACK_PHYS_BASE_FIELD);
  info.stack_size = load_be_octa(wire, MMIX_BOOTINFO_STACK_SIZE_FIELD);
  if (!valid_memory_layout(&info))
    return MMIX_BOOTINFO_BAD_MEMORY;

  info.mmio_base = load_be_octa(wire, MMIX_BOOTINFO_MMIO_BASE_FIELD);
  info.uart_base = load_be_octa(wire, MMIX_BOOTINFO_UART_BASE_FIELD);
  info.uart_irq = load_be_octa(wire, MMIX_BOOTINFO_UART_IRQ_FIELD);
  info.timer_base = load_be_octa(wire, MMIX_BOOTINFO_TIMER_BASE_FIELD);
  info.timer_irq_base =
      load_be_octa(wire, MMIX_BOOTINFO_TIMER_IRQ_BASE_FIELD);
  info.timer_irq_count =
      load_be_octa(wire, MMIX_BOOTINFO_TIMER_IRQ_COUNT_FIELD);
  info.intc_base = load_be_octa(wire, MMIX_BOOTINFO_INTC_BASE_FIELD);
  info.intc_irq_count =
      load_be_octa(wire, MMIX_BOOTINFO_INTC_IRQ_COUNT_FIELD);
  info.virtio_mmio_base =
      load_be_octa(wire, MMIX_BOOTINFO_VIRTIO_MMIO_BASE_FIELD);
  info.virtio_mmio_irq =
      load_be_octa(wire, MMIX_BOOTINFO_VIRTIO_MMIO_IRQ_FIELD);
  info.virtio_mmio_count =
      load_be_octa(wire, MMIX_BOOTINFO_VIRTIO_MMIO_COUNT_FIELD);
  if (!valid_devices(&info))
    return MMIX_BOOTINFO_BAD_DEVICE;

  // Keep this copy independent of compiler-generated freestanding memcpy.
  decoded->cpu_count = info.cpu_count;
  decoded->boot_cpu_id = info.boot_cpu_id;
  decoded->ram_base = info.ram_base;
  decoded->ram_size = info.ram_size;
  decoded->low_ram_base = info.low_ram_base;
  decoded->low_ram_size = info.low_ram_size;
  decoded->pool_logical_base = info.pool_logical_base;
  decoded->pool_phys_base = info.pool_phys_base;
  decoded->pool_size = info.pool_size;
  decoded->data_logical_base = info.data_logical_base;
  decoded->data_phys_base = info.data_phys_base;
  decoded->data_size = info.data_size;
  decoded->stack_logical_base = info.stack_logical_base;
  decoded->stack_phys_base = info.stack_phys_base;
  decoded->stack_size = info.stack_size;
  decoded->mmio_base = info.mmio_base;
  decoded->uart_base = info.uart_base;
  decoded->uart_irq = info.uart_irq;
  decoded->timer_base = info.timer_base;
  decoded->timer_irq_base = info.timer_irq_base;
  decoded->timer_irq_count = info.timer_irq_count;
  decoded->intc_base = info.intc_base;
  decoded->intc_irq_count = info.intc_irq_count;
  decoded->virtio_mmio_base = info.virtio_mmio_base;
  decoded->virtio_mmio_irq = info.virtio_mmio_irq;
  decoded->virtio_mmio_count = info.virtio_mmio_count;
  return MMIX_BOOTINFO_OK;
}
