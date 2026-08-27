#ifndef XV6_MMIX_BOOTINFO_H
#define XV6_MMIX_BOOTINFO_H

#define MMIX_BOOTINFO_MAGIC 0x4d4d4958424f4f54
#define MMIX_BOOTINFO_VERSION 1
#define MMIX_BOOTINFO_OCTA_SIZE 8

// Version 1 is a table of unsigned big-endian octas. These values are wire
// indices, not the layout of a C structure.
enum mmix_bootinfo_field {
  MMIX_BOOTINFO_MAGIC_FIELD = 0,
  MMIX_BOOTINFO_VERSION_FIELD = 1,
  MMIX_BOOTINFO_SIZE_FIELD = 2,
  MMIX_BOOTINFO_FLAGS_FIELD = 3,
  MMIX_BOOTINFO_CPU_COUNT_FIELD = 4,
  MMIX_BOOTINFO_BOOT_CPU_ID_FIELD = 5,
  MMIX_BOOTINFO_RAM_BASE_FIELD = 6,
  MMIX_BOOTINFO_RAM_SIZE_FIELD = 7,
  MMIX_BOOTINFO_LOW_RAM_BASE_FIELD = 8,
  MMIX_BOOTINFO_LOW_RAM_SIZE_FIELD = 9,
  MMIX_BOOTINFO_POOL_LOGICAL_BASE_FIELD = 10,
  MMIX_BOOTINFO_POOL_PHYS_BASE_FIELD = 11,
  MMIX_BOOTINFO_POOL_SIZE_FIELD = 12,
  MMIX_BOOTINFO_DATA_LOGICAL_BASE_FIELD = 13,
  MMIX_BOOTINFO_DATA_PHYS_BASE_FIELD = 14,
  MMIX_BOOTINFO_DATA_SIZE_FIELD = 15,
  MMIX_BOOTINFO_STACK_LOGICAL_BASE_FIELD = 16,
  MMIX_BOOTINFO_STACK_PHYS_BASE_FIELD = 17,
  MMIX_BOOTINFO_STACK_SIZE_FIELD = 18,
  MMIX_BOOTINFO_MMIO_BASE_FIELD = 19,
  MMIX_BOOTINFO_UART_BASE_FIELD = 20,
  MMIX_BOOTINFO_UART_IRQ_FIELD = 21,
  MMIX_BOOTINFO_TIMER_BASE_FIELD = 22,
  MMIX_BOOTINFO_TIMER_IRQ_BASE_FIELD = 23,
  MMIX_BOOTINFO_TIMER_IRQ_COUNT_FIELD = 24,
  MMIX_BOOTINFO_INTC_BASE_FIELD = 25,
  MMIX_BOOTINFO_INTC_IRQ_COUNT_FIELD = 26,
  MMIX_BOOTINFO_VIRTIO_MMIO_BASE_FIELD = 27,
  MMIX_BOOTINFO_VIRTIO_MMIO_IRQ_FIELD = 28,
  MMIX_BOOTINFO_VIRTIO_MMIO_COUNT_FIELD = 29,
  MMIX_BOOTINFO_FRAMEBUFFER_CONTROL_BASE_FIELD = 30,
  MMIX_BOOTINFO_FRAMEBUFFER_BASE_FIELD = 31,
  MMIX_BOOTINFO_FRAMEBUFFER_SIZE_FIELD = 32,
  MMIX_BOOTINFO_FRAMEBUFFER_IRQ_FIELD = 33,
  MMIX_BOOTINFO_FRAMEBUFFER_WIDTH_FIELD = 34,
  MMIX_BOOTINFO_FRAMEBUFFER_HEIGHT_FIELD = 35,
  MMIX_BOOTINFO_FRAMEBUFFER_STRIDE_FIELD = 36,
  MMIX_BOOTINFO_FRAMEBUFFER_FORMAT_FIELD = 37,
  MMIX_BOOTINFO_KERNEL_CMDLINE_ADDR_FIELD = 38,
  MMIX_BOOTINFO_KERNEL_CMDLINE_SIZE_FIELD = 39,
  MMIX_BOOTINFO_IPI_BASE_FIELD = 40,
  MMIX_BOOTINFO_IPI_TARGET_COUNT_FIELD = 41,
  MMIX_BOOTINFO_IPI_REQUEST_MASK_FIELD = 42,
  MMIX_BOOTINFO_HIGH_RAM_BASE_FIELD = 43,
  MMIX_BOOTINFO_HIGH_RAM_SIZE_FIELD = 44,
  MMIX_BOOTINFO_FIELD_COUNT = 45,
};

#define MMIX_BOOTINFO_FIELD_OFFSET(field)                                   \
  ((field) * MMIX_BOOTINFO_OCTA_SIZE)
#define MMIX_BOOTINFO_MIN_SIZE                                              \
  MMIX_BOOTINFO_FIELD_OFFSET(MMIX_BOOTINFO_FIELD_COUNT)

enum mmix_bootinfo_status {
  MMIX_BOOTINFO_OK = 0,
  MMIX_BOOTINFO_BAD_ARGUMENT = -1,
  MMIX_BOOTINFO_BAD_ADDRESS = -2,
  MMIX_BOOTINFO_BAD_HEADER = -3,
  MMIX_BOOTINFO_BAD_CPU = -4,
  MMIX_BOOTINFO_BAD_MEMORY = -5,
  MMIX_BOOTINFO_BAD_DEVICE = -6,
};

struct mmix_physical_range {
  uint64 base;
  uint64 size;
};

enum mmix_physical_ram_range {
  MMIX_PHYSICAL_RAM_LOW,
  MMIX_PHYSICAL_RAM_HIGH,
  MMIX_PHYSICAL_RAM_RANGE_COUNT,
};

struct mmix_physical_memory {
  uint64 total_size;
  uint64 range_count;
  struct mmix_physical_range range[MMIX_PHYSICAL_RAM_RANGE_COUNT];
};

// Decoded platform information used by early boot. QEMU's RAM wire fields are
// normalized into one kernel-owned physical topology.
struct mmix_bootinfo {
  uint64 cpu_count;
  uint64 boot_cpu_id;

  struct mmix_physical_memory memory;
  uint64 low_ram_base;
  uint64 low_ram_size;

  uint64 pool_logical_base;
  uint64 pool_phys_base;
  uint64 pool_size;
  uint64 data_logical_base;
  uint64 data_phys_base;
  uint64 data_size;
  uint64 stack_logical_base;
  uint64 stack_phys_base;
  uint64 stack_size;

  uint64 mmio_base;
  uint64 uart_base;
  uint64 uart_irq;
  uint64 timer_base;
  uint64 timer_irq_base;
  uint64 timer_irq_count;
  uint64 intc_base;
  uint64 intc_irq_count;
  uint64 virtio_mmio_base;
  uint64 virtio_mmio_irq;
  uint64 virtio_mmio_count;
  uint64 framebuffer_control_base;
  uint64 ipi_base;
  uint64 ipi_target_count;
  uint64 ipi_request_mask;
};

int bootinfo_decode(uint64 startup_cpu_id, uint64 bootinfo_pa,
                         struct mmix_bootinfo *decoded);

_Static_assert(sizeof(uint64) == MMIX_BOOTINFO_OCTA_SIZE,
               "MMIX boot-info octas require 64-bit uint64");
_Static_assert(MMIX_BOOTINFO_FIELD_OFFSET(MMIX_BOOTINFO_MAGIC_FIELD) == 0x000,
               "unexpected boot-info magic offset");
_Static_assert(MMIX_BOOTINFO_FIELD_OFFSET(MMIX_BOOTINFO_BOOT_CPU_ID_FIELD) ==
                   0x028,
               "unexpected boot CPU ID offset");
_Static_assert(MMIX_BOOTINFO_FIELD_OFFSET(MMIX_BOOTINFO_MMIO_BASE_FIELD) ==
                   0x098,
               "unexpected MMIO base offset");
_Static_assert(
    MMIX_BOOTINFO_FIELD_OFFSET(MMIX_BOOTINFO_VIRTIO_MMIO_COUNT_FIELD) == 0x0e8,
    "unexpected VirtIO count offset");
_Static_assert(
    MMIX_BOOTINFO_FIELD_OFFSET(MMIX_BOOTINFO_FRAMEBUFFER_FORMAT_FIELD) == 0x128,
    "unexpected framebuffer format offset");
_Static_assert(
    MMIX_BOOTINFO_FIELD_OFFSET(MMIX_BOOTINFO_HIGH_RAM_BASE_FIELD) == 0x158,
    "unexpected high RAM base offset");
_Static_assert(
    MMIX_BOOTINFO_FIELD_OFFSET(MMIX_BOOTINFO_HIGH_RAM_SIZE_FIELD) == 0x160,
    "unexpected high RAM size offset");
_Static_assert(MMIX_BOOTINFO_FIELD_COUNT == 45,
               "unexpected version-1 boot-info field count");
_Static_assert(MMIX_BOOTINFO_MIN_SIZE == 0x168,
               "unexpected version-1 boot-info size");
_Static_assert((MMIX_BOOTINFO_MIN_SIZE % MMIX_BOOTINFO_OCTA_SIZE) == 0,
               "boot-info size must be octa-aligned");

#endif
