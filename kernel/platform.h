#ifndef XV6_MMIX_PLATFORM_H
#define XV6_MMIX_PLATFORM_H

#include "param.h"
#include "types.h"

#define INITIAL_REGISTER_STACK_SIZE 0x00008000UL
#define PLATFORM_RAM_MIN_SIZE       0x08000000UL
#define PLATFORM_RAM_MAX_SIZE       0x40000000UL
#define PLATFORM_FRAMEBUFFER_SIZE   0x00300000UL
#define PLATFORM_MAX_RESERVATIONS   (NCPU + 2)
#define PLATFORM_NO_CPU             (~0U)
#define PLATFORM_VIRTIO_SLOTS       32U

enum platform_status {
  PLATFORM_OK = 0,
  PLATFORM_BAD_ARGUMENT = -1,
  PLATFORM_BAD_ROOT = -2,
  PLATFORM_BAD_CPU_BUS = -3,
  PLATFORM_BAD_CPU = -4,
  PLATFORM_TOO_MANY_CPUS = -5,
  PLATFORM_BAD_CPU_IDS = -6,
  PLATFORM_BAD_PHANDLE = -7,
  PLATFORM_BAD_MEMORY = -8,
  PLATFORM_BAD_REGISTER_STACK = -9,
  PLATFORM_REGISTER_STACK_OUTSIDE_RAM = -10,
  PLATFORM_REGISTER_STACK_OVERLAP = -11,
  PLATFORM_BAD_RESERVED_MEMORY = -12,
  PLATFORM_BAD_FDT_RESERVATION = -13,
  PLATFORM_BAD_FRAMEBUFFER_MEMORY = -14,
  PLATFORM_RESERVATION_OVERLAP = -15,
  PLATFORM_UNSUPPORTED_INITRD = -16,
  PLATFORM_BAD_SOC = -17,
  PLATFORM_BAD_INTERRUPT_CONTROLLER = -18,
  PLATFORM_BAD_UART = -19,
  PLATFORM_BAD_TIMER = -20,
  PLATFORM_BAD_IPI = -21,
  PLATFORM_BAD_VIRTIO = -22,
  PLATFORM_BAD_FRAMEBUFFER_DEVICE = -23,
  PLATFORM_BAD_DEVICE_REFERENCE = -24,
  PLATFORM_NOT_READY = -25,
  PLATFORM_ALREADY_DISCOVERED = -26,
};

enum platform_reservation_owner {
  PLATFORM_RESERVATION_FDT,
  PLATFORM_RESERVATION_CPU_REGISTER_STACK,
  PLATFORM_RESERVATION_FRAMEBUFFER,
};

enum platform_reservation_lifetime {
  PLATFORM_RESERVATION_UNTIL_PLATFORM_COPIED,
  PLATFORM_RESERVATION_UNTIL_CPU_RELEASED,
  PLATFORM_RESERVATION_DEVICE_LIFETIME,
};

// Public query results are copies. Their addresses and all ranges they contain
// are physical addresses; callers must not treat them as CPU aliases.
struct platform_physical_range {
  uint64 physical_base;
  uint64 size;
};

struct platform_reservation_info {
  struct platform_physical_range physical;
  enum platform_reservation_owner owner;
  enum platform_reservation_lifetime lifetime;
  uint32 cpu_id;
};

struct platform_intc_config {
  struct platform_physical_range physical_global;
  struct platform_physical_range physical_contexts;
  uint32 source_count;
  uint32 context_count;
  uint32 context_stride;
};

struct platform_uart_config {
  struct platform_physical_range physical_registers;
  uint32 interrupt;
  uint32 clock_frequency;
  uint32 baud_rate;
  uint32 register_shift;
  uint32 register_width;
};

struct platform_timer_config {
  struct platform_physical_range physical_global;
  struct platform_physical_range physical_contexts;
  uint32 context_count;
  uint32 context_stride;
  uint32 clock_frequency;
};

struct platform_ipi_config {
  struct platform_physical_range physical_global;
  struct platform_physical_range physical_contexts;
  uint32 context_count;
  uint32 context_stride;
  uint32 request_bit;
};

struct platform_virtio_config {
  struct platform_physical_range physical_registers;
  uint32 interrupt;
};

struct platform_framebuffer_config {
  struct platform_physical_range physical_control;
  struct platform_physical_range physical_memory;
};

struct fdt;

int platform_discover(const struct fdt *, uint64);

// Discovery and publication must complete before these queries are used.
// Aggregate results are copied through output parameters so no internal
// platform storage escapes and the interface does not depend on structure-
// return ABI support.
uint64 platform_fdt_physical_address(void);
uint32 platform_cpu_count(void);
uint64 platform_cpu_mask(void);
int platform_cpu_initial_stack(uint32, struct platform_physical_range *);
int platform_cpu_initial_stack_contains(uint32, uint64);
int platform_ram(struct platform_physical_range *);
uint32 platform_reservation_count(void);
int platform_reservation(uint32, struct platform_reservation_info *);
int platform_intc_config(struct platform_intc_config *);
int platform_uart_config(struct platform_uart_config *);
int platform_timer_config(struct platform_timer_config *);
int platform_timer_interrupt(uint32, uint32 *);
int platform_ipi_config(struct platform_ipi_config *);
uint32 platform_virtio_count(void);
int platform_virtio_config(uint32, struct platform_virtio_config *);
int platform_framebuffer_config(struct platform_framebuffer_config *);

#endif
