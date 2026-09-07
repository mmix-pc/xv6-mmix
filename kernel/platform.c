#include "types.h"
#include "fdt.h"
#include "memlayout.h"
#include "platform.h"

// FIXME: Move this immutable state into the platform module after every
// consumer uses the query interface.
extern struct platform mmix_platform;
extern uint64 mmix_fdt_address;

enum node_role {
  NODE_OTHER,
  NODE_ROOT,
  NODE_CPUS,
  NODE_CPU,
  NODE_MEMORY,
  NODE_RESERVED_MEMORY,
  NODE_RESERVED_CHILD,
  NODE_CHOSEN,
  NODE_ROOT_OTHER,
};

enum {
  PLATFORM_DECODE_DEPTHS = 3,
};

struct property_set {
  int address_cells;
  int size_cells;
  int compatible;
  int device_type;
  int status;
  int enable_method;
  int reg;
  int phandle;
  int linux_phandle;
  int initial_stack;
  int cpu;
  int ranges;
  int no_map;
  int initrd_start;
  int initrd_end;
  uint32 address_cells_value;
  uint32 size_cells_value;
  uint32 reg_value;
  uint32 phandle_value;
  uint32 linux_phandle_value;
  uint32 initial_stack_value;
  uint32 cpu_value;
  uint64 initrd_start_value;
  uint64 initrd_end_value;
  uint64 range_start;
  uint64 range_size;
};

struct raw_cpu {
  uint32 id;
  uint32 phandle;
  uint32 stack_phandle;
};

struct raw_stack {
  uint32 phandle;
  uint32 cpu_phandle;
  uint64 start;
  uint64 size;
};

struct raw_framebuffer {
  uint32 phandle;
  uint64 start;
  uint64 size;
};

struct topology_decoder {
  struct raw_cpu cpus[NCPU];
  struct raw_stack stacks[NCPU];
  struct property_set properties[PLATFORM_DECODE_DEPTHS];
  enum node_role roles[PLATFORM_DECODE_DEPTHS];
  uint32 cpu_count;
  uint32 stack_count;
  uint32 cpus_nodes;
  uint32 memory_nodes;
  uint32 reserved_nodes;
  uint32 chosen_nodes;
  uint32 framebuffer_count;
  uint32 unknown_reserved_children;
  uint64 ram_size;
  struct raw_framebuffer framebuffer;
  int has_initrd_start;
  int has_initrd_end;
};

static int
same_name(const uint8 *name, uint32 size, const char *expected)
{
  uint32 expected_size = 0;

  while (expected[expected_size] != 0)
    expected_size++;
  if (size != expected_size)
    return 0;
  for (uint32 i = 0; i < size; i++) {
    if (name[i] != (uint8)expected[i])
      return 0;
  }
  return 1;
}

static int
string_value(const uint8 *value, uint32 size, const char *expected)
{
  uint32 expected_size = 0;

  while (expected[expected_size] != 0)
    expected_size++;
  return size == expected_size + 1 && value[expected_size] == 0 &&
         same_name(value, expected_size, expected);
}

static int
string_list_contains(const uint8 *value, uint32 size, const char *expected)
{
  uint32 offset = 0;
  int found = 0;

  while (offset < size) {
    uint32 start = offset;

    while (offset < size && value[offset] != 0)
      offset++;
    if (offset == size || offset == start)
      return 0;
    if (same_name(value + start, offset - start, expected))
      found = 1;
    offset++;
  }
  return found;
}

static int
read_cell(const struct fdt_event *event, uint32 *value)
{
  return event->value_size == 4 &&
         fdt_read_u32(event->value, event->value_size, value) == FDT_OK;
}

static int
read_range(const struct fdt_event *event, uint64 *start, uint64 *size)
{
  return event->value_size == 16 &&
         fdt_read_u64(event->value, event->value_size, start) == FDT_OK &&
         fdt_read_u64(event->value + 8, event->value_size - 8, size) ==
           FDT_OK;
}

static enum node_role
node_role(const struct topology_decoder *decoder, const struct fdt_event *event)
{
  if (event->depth == 0)
    return NODE_ROOT;
  if (event->depth == 1) {
    if (same_name(event->name, event->name_size, "cpus"))
      return NODE_CPUS;
    if (same_name(event->name, event->name_size, "memory@0"))
      return NODE_MEMORY;
    if (same_name(event->name, event->name_size, "reserved-memory"))
      return NODE_RESERVED_MEMORY;
    if (same_name(event->name, event->name_size, "chosen"))
      return NODE_CHOSEN;
    return NODE_ROOT_OTHER;
  }
  if (event->depth == 2 && decoder->roles[1] == NODE_CPUS)
    return NODE_CPU;
  if (event->depth == 2 && decoder->roles[1] == NODE_RESERVED_MEMORY)
    return NODE_RESERVED_CHILD;
  return NODE_OTHER;
}

static void
record_property(struct property_set *properties,
                const struct fdt_event *event)
{
  if (same_name(event->name, event->name_size, "#address-cells")) {
    properties->address_cells = read_cell(event,
                                           &properties->address_cells_value);
  } else if (same_name(event->name, event->name_size, "#size-cells")) {
    properties->size_cells = read_cell(event, &properties->size_cells_value);
  } else if (same_name(event->name, event->name_size, "compatible")) {
    properties->compatible = string_list_contains(
      event->value, event->value_size, "qemu,mmix-cpu") ? 1 :
      string_list_contains(event->value, event->value_size,
                           "qemu,mmix-register-stack") ? 2 :
      string_list_contains(event->value, event->value_size,
                           "qemu,mmix-framebuffer-memory") ? 3 : -1;
  } else if (same_name(event->name, event->name_size, "device_type")) {
    properties->device_type = string_value(event->value, event->value_size,
                                            "cpu") ? 1 :
                              string_value(event->value, event->value_size,
                                           "memory") ? 2 : -1;
  } else if (same_name(event->name, event->name_size, "status")) {
    properties->status = string_value(event->value, event->value_size,
                                      "okay") ? 1 : -1;
  } else if (same_name(event->name, event->name_size, "enable-method")) {
    properties->enable_method = string_value(
      event->value, event->value_size, "qemu,mmix-immediate-entry") ? 1 : -1;
  } else if (same_name(event->name, event->name_size, "reg")) {
    properties->reg = read_cell(event, &properties->reg_value) ? 1 :
                      read_range(event, &properties->range_start,
                                 &properties->range_size) ? 2 : -1;
  } else if (same_name(event->name, event->name_size, "phandle")) {
    properties->phandle = read_cell(event, &properties->phandle_value) ?
                            1 : -1;
  } else if (same_name(event->name, event->name_size, "linux,phandle")) {
    properties->linux_phandle = read_cell(
      event, &properties->linux_phandle_value) ? 1 : -1;
  } else if (same_name(event->name, event->name_size,
                       "qemu,initial-register-stack")) {
    properties->initial_stack = read_cell(
      event, &properties->initial_stack_value) ? 1 : -1;
  } else if (same_name(event->name, event->name_size, "qemu,cpu")) {
    properties->cpu = read_cell(event, &properties->cpu_value) ? 1 : -1;
  } else if (same_name(event->name, event->name_size, "ranges")) {
    properties->ranges = event->value_size == 0 ? 1 : -1;
  } else if (same_name(event->name, event->name_size, "no-map")) {
    properties->no_map = event->value_size == 0 ? 1 : -1;
  } else if (same_name(event->name, event->name_size,
                       "linux,initrd-start")) {
    properties->initrd_start =
      event->value_size == 8 &&
      fdt_read_u64(event->value, event->value_size,
                   &properties->initrd_start_value) == FDT_OK ? 1 : -1;
  } else if (same_name(event->name, event->name_size,
                       "linux,initrd-end")) {
    properties->initrd_end =
      event->value_size == 8 &&
      fdt_read_u64(event->value, event->value_size,
                   &properties->initrd_end_value) == FDT_OK ? 1 : -1;
  }
}

static int
close_node(struct topology_decoder *decoder, uint32 depth)
{
  struct property_set *properties = &decoder->properties[depth];

  switch (decoder->roles[depth]) {
  case NODE_ROOT:
    if (properties->address_cells != 1 ||
        properties->address_cells_value != 2 || properties->size_cells != 1 ||
        properties->size_cells_value != 2)
      return PLATFORM_BAD_ROOT;
    break;
  case NODE_CPUS:
    decoder->cpus_nodes++;
    if (properties->address_cells != 1 ||
        properties->address_cells_value != 1 || properties->size_cells != 1 ||
        properties->size_cells_value != 0)
      return PLATFORM_BAD_CPU_BUS;
    break;
  case NODE_CPU:
    if (decoder->cpu_count == NCPU)
      return PLATFORM_TOO_MANY_CPUS;
    if (properties->compatible != 1 || properties->device_type != 1 ||
        properties->status != 1 || properties->enable_method != 1 ||
        properties->reg != 1 || properties->phandle != 1 ||
        properties->linux_phandle != 1 || properties->initial_stack != 1)
      return PLATFORM_BAD_CPU;
    if (properties->phandle_value == 0 ||
        properties->phandle_value != properties->linux_phandle_value)
      return PLATFORM_BAD_PHANDLE;
    decoder->cpus[decoder->cpu_count++] = (struct raw_cpu) {
      .id = properties->reg_value,
      .phandle = properties->phandle_value,
      .stack_phandle = properties->initial_stack_value,
    };
    break;
  case NODE_MEMORY:
    decoder->memory_nodes++;
    if (properties->device_type != 2 || properties->reg != 2 ||
        properties->range_start != 0 || properties->range_size == 0)
      return PLATFORM_BAD_MEMORY;
    decoder->ram_size = properties->range_size;
    break;
  case NODE_RESERVED_MEMORY:
    decoder->reserved_nodes++;
    if (properties->address_cells != 1 ||
        properties->address_cells_value != 2 || properties->size_cells != 1 ||
        properties->size_cells_value != 2 || properties->ranges != 1)
      return PLATFORM_BAD_RESERVED_MEMORY;
    break;
  case NODE_RESERVED_CHILD:
    if (properties->compatible == 3) {
      decoder->framebuffer_count++;
      if (properties->reg != 2 || properties->phandle != 1 ||
          properties->linux_phandle != 1 || properties->no_map != 1 ||
          properties->phandle_value == 0 ||
          properties->phandle_value != properties->linux_phandle_value)
        return PLATFORM_BAD_FRAMEBUFFER_MEMORY;
      decoder->framebuffer = (struct raw_framebuffer) {
        .phandle = properties->phandle_value,
        .start = properties->range_start,
        .size = properties->range_size,
      };
      break;
    }
    if (properties->compatible != 2) {
      decoder->unknown_reserved_children++;
      break;
    }
    if (decoder->stack_count == NCPU)
      return PLATFORM_BAD_REGISTER_STACK;
    if (properties->reg != 2 || properties->phandle != 1 ||
        properties->linux_phandle != 1 || properties->cpu != 1 ||
        properties->phandle_value == 0 ||
        properties->phandle_value != properties->linux_phandle_value)
      return PLATFORM_BAD_REGISTER_STACK;
    decoder->stacks[decoder->stack_count++] = (struct raw_stack) {
      .phandle = properties->phandle_value,
      .cpu_phandle = properties->cpu_value,
      .start = properties->range_start,
      .size = properties->range_size,
    };
    break;
  case NODE_CHOSEN:
    decoder->chosen_nodes++;
    if (properties->initrd_start != 0 || properties->initrd_end != 0) {
      decoder->has_initrd_start = properties->initrd_start;
      decoder->has_initrd_end = properties->initrd_end;
    }
    break;
  case NODE_ROOT_OTHER:
    if (properties->device_type == 2)
      return PLATFORM_BAD_MEMORY;
    break;
  case NODE_OTHER:
    break;
  }
  return PLATFORM_OK;
}

static int
decode_nodes(const struct fdt *fdt, struct topology_decoder *decoder)
{
  struct fdt_iterator iterator;
  struct fdt_event event;
  int status;

  fdt_iterator_init(fdt, &iterator);
  for (;;) {
    status = fdt_next(fdt, &iterator, &event);
    if (status != FDT_OK)
      return PLATFORM_BAD_ARGUMENT;
    if (event.type == FDT_EVENT_BEGIN_NODE) {
      if (event.depth < PLATFORM_DECODE_DEPTHS) {
        decoder->roles[event.depth] = node_role(decoder, &event);
        decoder->properties[event.depth] = (struct property_set) { 0 };
      }
    } else if (event.type == FDT_EVENT_PROPERTY) {
      if (event.depth < PLATFORM_DECODE_DEPTHS)
        record_property(&decoder->properties[event.depth], &event);
    } else if (event.type == FDT_EVENT_END_NODE) {
      if (event.depth < PLATFORM_DECODE_DEPTHS) {
        status = close_node(decoder, event.depth);
        if (status != PLATFORM_OK)
          return status;
      }
    } else {
      return PLATFORM_OK;
    }
  }
}

struct handle_properties {
  int phandle;
  int linux_phandle;
  uint32 phandle_value;
  uint32 linux_phandle_value;
};

static int
phandle_is_unique(const struct fdt *fdt, uint32 target)
{
  struct handle_properties properties[FDT_MAX_DEPTH] = { 0 };
  struct fdt_iterator iterator;
  struct fdt_event event;
  uint32 matches = 0;
  int status;

  fdt_iterator_init(fdt, &iterator);
  for (;;) {
    status = fdt_next(fdt, &iterator, &event);
    if (status != FDT_OK)
      return PLATFORM_BAD_ARGUMENT;
    if (event.type == FDT_EVENT_BEGIN_NODE) {
      properties[event.depth] = (struct handle_properties) { 0 };
    } else if (event.type == FDT_EVENT_PROPERTY) {
      struct handle_properties *node = &properties[event.depth];

      if (same_name(event.name, event.name_size, "phandle"))
        node->phandle = read_cell(&event, &node->phandle_value) ? 1 : -1;
      else if (same_name(event.name, event.name_size, "linux,phandle"))
        node->linux_phandle = read_cell(
          &event, &node->linux_phandle_value) ? 1 : -1;
    } else if (event.type == FDT_EVENT_END_NODE) {
      struct handle_properties *node = &properties[event.depth];

      if ((node->phandle == 1 && node->phandle_value == target) ||
          (node->linux_phandle == 1 &&
           node->linux_phandle_value == target)) {
        if (node->phandle != 1 || node->linux_phandle != 1 || target == 0 ||
            node->phandle_value != target ||
            node->linux_phandle_value != target || ++matches > 1)
          return PLATFORM_BAD_PHANDLE;
      }
    } else {
      return matches == 1 ? PLATFORM_OK : PLATFORM_BAD_PHANDLE;
    }
  }
}

static int
validate_phandles(const struct fdt *fdt,
                  const struct topology_decoder *decoder)
{
  for (uint32 i = 0; i < decoder->cpu_count; i++) {
    if (decoder->cpus[i].stack_phandle == 0 ||
        phandle_is_unique(fdt, decoder->cpus[i].phandle) != PLATFORM_OK)
      return PLATFORM_BAD_PHANDLE;
    for (uint32 j = 0; j < i; j++) {
      if (decoder->cpus[i].phandle == decoder->cpus[j].phandle)
        return PLATFORM_BAD_PHANDLE;
    }
  }
  for (uint32 i = 0; i < decoder->stack_count; i++) {
    if (decoder->stacks[i].cpu_phandle == 0 ||
        phandle_is_unique(fdt, decoder->stacks[i].phandle) != PLATFORM_OK)
      return PLATFORM_BAD_PHANDLE;
    for (uint32 j = 0; j < decoder->cpu_count; j++) {
      if (decoder->stacks[i].phandle == decoder->cpus[j].phandle)
        return PLATFORM_BAD_PHANDLE;
    }
    for (uint32 j = 0; j < i; j++) {
      if (decoder->stacks[i].phandle == decoder->stacks[j].phandle)
        return PLATFORM_BAD_PHANDLE;
    }
  }
  return PLATFORM_OK;
}

static int
normalize_topology(const struct fdt *fdt,
                   const struct topology_decoder *decoder,
                   struct platform_cpu_topology *topology)
{
  struct platform_cpu_topology result = { 0 };
  int status;

  if (decoder->cpus_nodes != 1)
    return PLATFORM_BAD_CPU_BUS;
  if (decoder->memory_nodes != 1)
    return PLATFORM_BAD_MEMORY;
  if (decoder->reserved_nodes != 1)
    return PLATFORM_BAD_REGISTER_STACK;
  if (decoder->cpu_count == 0)
    return PLATFORM_BAD_CPU_IDS;
  if (decoder->stack_count != decoder->cpu_count)
    return PLATFORM_BAD_REGISTER_STACK;
  status = validate_phandles(fdt, decoder);
  if (status != PLATFORM_OK)
    return status;

  result.count = decoder->cpu_count;
  for (uint32 i = 0; i < decoder->cpu_count; i++) {
    const struct raw_cpu *cpu = 0;
    const struct raw_stack *stack = 0;

    for (uint32 j = 0; j < decoder->cpu_count; j++) {
      if (decoder->cpus[j].id == i) {
        if (cpu != 0)
          return PLATFORM_BAD_CPU_IDS;
        cpu = &decoder->cpus[j];
      }
    }
    if (cpu == 0)
      return PLATFORM_BAD_CPU_IDS;
    for (uint32 j = 0; j < decoder->stack_count; j++) {
      if (decoder->stacks[j].phandle == cpu->stack_phandle) {
        if (stack != 0)
          return PLATFORM_BAD_PHANDLE;
        stack = &decoder->stacks[j];
      }
    }
    if (stack == 0 || stack->cpu_phandle != cpu->phandle)
      return PLATFORM_BAD_PHANDLE;
    if (stack->size != INITIAL_REGISTER_STACK_SIZE ||
        (stack->start & (MMIX_PAGE_SIZE - 1)) != 0)
      return PLATFORM_BAD_REGISTER_STACK;
    if (stack->start > decoder->ram_size ||
        stack->size > decoder->ram_size - stack->start)
      return PLATFORM_REGISTER_STACK_OUTSIDE_RAM;
    for (uint32 j = 0; j < i; j++) {
      uint64 other_start = result.cpus[j].initial_register_stack;
      uint64 other_size = result.cpus[j].initial_register_stack_size;

      if ((stack->start < other_start &&
           stack->size > other_start - stack->start) ||
          (stack->start >= other_start &&
           other_size > stack->start - other_start))
        return PLATFORM_REGISTER_STACK_OVERLAP;
    }
    result.cpus[i] = (struct platform_cpu) {
      .id = i,
      .initial_register_stack = stack->start,
      .initial_register_stack_size = stack->size,
    };
  }
  *topology = result;
  return PLATFORM_OK;
}

static int
add_reservation(struct platform_memory *memory, uint64 start, uint64 size,
                enum platform_reservation_owner owner,
                enum platform_reservation_lifetime lifetime, uint32 cpu_id)
{
  if (memory->reservation_count == PLATFORM_MAX_RESERVATIONS || size == 0 ||
      start > memory->ram_size || size > memory->ram_size - start)
    return PLATFORM_BAD_RESERVED_MEMORY;
  for (uint32 i = 0; i < memory->reservation_count; i++) {
    const struct platform_reservation *other = &memory->reservations[i];

    if ((start < other->start && size > other->start - start) ||
        (start >= other->start && other->size > start - other->start))
      return PLATFORM_RESERVATION_OVERLAP;
  }
  memory->reservations[memory->reservation_count++] =
    (struct platform_reservation) {
      .start = start,
      .size = size,
      .owner = owner,
      .lifetime = lifetime,
      .cpu_id = cpu_id,
    };
  return PLATFORM_OK;
}

static int
normalize_memory(const struct fdt *fdt, uint64 fdt_address,
                 const struct topology_decoder *decoder,
                 const struct platform_cpu_topology *topology,
                 struct platform_memory *memory)
{
  uint64 reservation_start;
  uint64 reservation_size;
  int status;

  if (decoder->memory_nodes != 1 ||
      decoder->ram_size < PLATFORM_RAM_MIN_SIZE ||
      decoder->ram_size > PLATFORM_RAM_MAX_SIZE ||
      (decoder->ram_size & (MMIX_PAGE_SIZE - 1)) != 0)
    return PLATFORM_BAD_MEMORY;
  if (decoder->reserved_nodes != 1 ||
      decoder->unknown_reserved_children != 0)
    return PLATFORM_BAD_RESERVED_MEMORY;
  if (decoder->chosen_nodes != 1)
    return PLATFORM_UNSUPPORTED_INITRD;
  if (decoder->has_initrd_start != 0 || decoder->has_initrd_end != 0)
    return PLATFORM_UNSUPPORTED_INITRD;
  if ((fdt_address & 7) != 0 || fdt->reservation_count != 1 ||
      fdt_reservation(fdt, 0, &reservation_start, &reservation_size) != FDT_OK ||
      reservation_start != fdt_address || reservation_size != fdt->total_size)
    return PLATFORM_BAD_FDT_RESERVATION;
  if (decoder->framebuffer_count != 1 ||
      decoder->framebuffer.size != PLATFORM_FRAMEBUFFER_SIZE ||
      (decoder->framebuffer.start & (MMIX_PAGE_SIZE - 1)) != 0 ||
      phandle_is_unique(fdt, decoder->framebuffer.phandle) != PLATFORM_OK)
    return PLATFORM_BAD_FRAMEBUFFER_MEMORY;

  *memory = (struct platform_memory) {
    .ram_start = 0,
    .ram_size = decoder->ram_size,
  };
  status = add_reservation(
    memory, reservation_start, reservation_size, PLATFORM_RESERVATION_FDT,
    PLATFORM_RESERVATION_UNTIL_PLATFORM_COPIED, PLATFORM_NO_CPU);
  if (status != PLATFORM_OK)
    return status == PLATFORM_BAD_RESERVED_MEMORY ?
             PLATFORM_BAD_FDT_RESERVATION : status;
  for (uint32 i = 0; i < topology->count; i++) {
    status = add_reservation(
      memory, topology->cpus[i].initial_register_stack,
      topology->cpus[i].initial_register_stack_size,
      PLATFORM_RESERVATION_CPU_REGISTER_STACK,
      PLATFORM_RESERVATION_UNTIL_CPU_RELEASED, i);
    if (status != PLATFORM_OK)
      return status;
  }
  status = add_reservation(
    memory, decoder->framebuffer.start, decoder->framebuffer.size,
    PLATFORM_RESERVATION_FRAMEBUFFER,
    PLATFORM_RESERVATION_DEVICE_LIFETIME, PLATFORM_NO_CPU);
  if (status == PLATFORM_BAD_RESERVED_MEMORY)
    return PLATFORM_BAD_FRAMEBUFFER_MEMORY;
  return status;
}

enum device_role {
  DEVICE_OTHER,
  DEVICE_SOC,
  DEVICE_SOC_CHILD,
  DEVICE_ALIASES,
  DEVICE_CHOSEN,
  DEVICE_CPUS,
  DEVICE_CPU,
  DEVICE_RESERVED_MEMORY,
  DEVICE_RESERVED_CHILD,
};

enum device_kind {
  DEVICE_KIND_UNKNOWN,
  DEVICE_KIND_SOC,
  DEVICE_KIND_INTC,
  DEVICE_KIND_UART,
  DEVICE_KIND_TIMER,
  DEVICE_KIND_IPI,
  DEVICE_KIND_VIRTIO,
  DEVICE_KIND_FRAMEBUFFER,
  DEVICE_KIND_FRAMEBUFFER_MEMORY,
};

enum {
  DEVICE_PATH_SIZE = 64,
};

struct device_property_set {
  enum device_kind compatible;
  int address_cells;
  int size_cells;
  int ranges_property;
  int reg_property;
  int interrupts_property;
  int affinity_property;
  int interrupt_parent;
  int phandle;
  int linux_phandle;
  int interrupt_controller;
  int interrupt_cells;
  int source_count;
  int context_count;
  int context_stride;
  int request_bit;
  int clock_frequency;
  int current_speed;
  int register_shift;
  int register_width;
  int memory_region;
  int serial0;
  int stdout_path;
  int node_name_valid;
  uint32 address_cells_value;
  uint32 size_cells_value;
  struct platform_mmio_range ranges[2];
  uint32 range_count;
  uint32 interrupts[NCPU];
  uint32 interrupt_count;
  uint32 affinities[NCPU];
  uint32 affinity_count;
  uint32 interrupt_parent_value;
  uint32 phandle_value;
  uint32 linux_phandle_value;
  uint32 interrupt_cells_value;
  uint32 source_count_value;
  uint32 context_count_value;
  uint32 context_stride_value;
  uint32 request_bit_value;
  uint32 clock_frequency_value;
  uint32 current_speed_value;
  uint32 register_shift_value;
  uint32 register_width_value;
  uint32 memory_region_value;
  uint32 reg_value;
  char node_name[DEVICE_PATH_SIZE];
  char serial0_value[DEVICE_PATH_SIZE];
  char stdout_path_value[DEVICE_PATH_SIZE];
};

struct device_cpu_handle {
  uint32 id;
  uint32 phandle;
};

struct raw_virtio {
  struct platform_mmio_range registers;
  int reg_property;
  uint32 range_count;
  int interrupts_property;
  uint32 interrupt_count;
  uint32 interrupt;
  int interrupt_parent;
  uint32 interrupt_parent_value;
};

struct device_decoder {
  enum device_role roles[PLATFORM_DECODE_DEPTHS];
  struct device_property_set properties[PLATFORM_DECODE_DEPTHS];
  struct device_cpu_handle cpus[NCPU];
  struct device_property_set intc;
  struct device_property_set uart;
  struct device_property_set timer;
  struct device_property_set ipi;
  struct device_property_set framebuffer;
  struct raw_virtio virtio[PLATFORM_VIRTIO_SLOTS];
  char serial0[DEVICE_PATH_SIZE];
  char stdout_path[DEVICE_PATH_SIZE];
  uint32 cpu_count;
  uint32 intc_count;
  uint32 uart_count;
  uint32 timer_count;
  uint32 ipi_count;
  uint32 framebuffer_count;
  uint32 framebuffer_memory_count;
  uint32 framebuffer_memory_phandle;
  uint32 virtio_count;
  uint32 soc_count;
  uint32 aliases_count;
  uint32 chosen_count;
};

// CPU 0 completes discovery before publishing the immutable platform data.
static struct device_decoder device_scratch;

#define REQUIRED_UART_BASE          0x0001000010000000UL
#define REQUIRED_FRAMEBUFFER_BASE   0x0001000018000000UL
#define REQUIRED_TIMER_BASE         0x0001000020000000UL
#define REQUIRED_TIMER_CONTEXT_BASE 0x0001000020010000UL
#define REQUIRED_IPI_BASE           0x0001000024000000UL
#define REQUIRED_IPI_CONTEXT_BASE   0x0001000024010000UL
#define REQUIRED_INTC_BASE          0x0001000030000000UL
#define REQUIRED_INTC_CONTEXT_BASE  0x0001000034000000UL
#define REQUIRED_VIRTIO_BASE        0x0001000040000000UL
#define REQUIRED_CONTEXT_STRIDE     0x00010000U
#define REQUIRED_INTC_SOURCES       8192U
#define REQUIRED_TIMER_SOURCE_BASE  16U
#define REQUIRED_VIRTIO_SOURCE_BASE 2048U

static int
copy_string(const struct fdt_event *event, char *output, uint32 capacity)
{
  if (event->value_size == 0 || event->value_size > capacity ||
      event->value[event->value_size - 1] != 0)
    return 0;
  for (uint32 i = 0; i + 1 < event->value_size; i++) {
    if (event->value[i] == 0)
      return 0;
    output[i] = event->value[i];
  }
  output[event->value_size - 1] = 0;
  return 1;
}

static int
copy_node_name(const struct fdt_event *event, char *output, uint32 capacity)
{
  if (event->name_size + 1 > capacity)
    return 0;
  for (uint32 i = 0; i < event->name_size; i++)
    output[i] = event->name[i];
  output[event->name_size] = 0;
  return 1;
}

static enum device_kind
device_compatible(const struct fdt_event *event)
{
  if (string_list_contains(event->value, event->value_size,
                           "qemu,mmix-intc"))
    return DEVICE_KIND_INTC;
  if (string_list_contains(event->value, event->value_size, "ns16550a"))
    return DEVICE_KIND_UART;
  if (string_list_contains(event->value, event->value_size,
                           "qemu,mmix-timer"))
    return DEVICE_KIND_TIMER;
  if (string_list_contains(event->value, event->value_size,
                           "qemu,mmix-ipi"))
    return DEVICE_KIND_IPI;
  if (string_list_contains(event->value, event->value_size, "virtio,mmio"))
    return DEVICE_KIND_VIRTIO;
  if (string_list_contains(event->value, event->value_size,
                           "qemu,mmix-framebuffer"))
    return DEVICE_KIND_FRAMEBUFFER;
  if (string_list_contains(event->value, event->value_size,
                           "qemu,mmix-framebuffer-memory"))
    return DEVICE_KIND_FRAMEBUFFER_MEMORY;
  if (string_list_contains(event->value, event->value_size, "simple-bus"))
    return DEVICE_KIND_SOC;
  return DEVICE_KIND_UNKNOWN;
}

static int
read_device_ranges(const struct fdt_event *event,
                   struct device_property_set *properties)
{
  uint32 count;

  if (event->value_size == 0 || event->value_size % 16 != 0)
    return 0;
  count = event->value_size / 16;
  if (count > 2)
    return 0;
  for (uint32 i = 0; i < count; i++) {
    if (fdt_read_u64(event->value + i * 16, 16,
                     &properties->ranges[i].start) != FDT_OK ||
        fdt_read_u64(event->value + i * 16 + 8, 8,
                     &properties->ranges[i].size) != FDT_OK ||
        properties->ranges[i].size == 0)
      return 0;
  }
  properties->range_count = count;
  return 1;
}

static int
read_device_cells(const struct fdt_event *event, uint32 *values,
                  uint32 *count)
{
  if (event->value_size == 0 || event->value_size % 4 != 0 ||
      event->value_size / 4 > NCPU)
    return 0;
  *count = event->value_size / 4;
  for (uint32 i = 0; i < *count; i++) {
    if (fdt_read_u32(event->value + i * 4, 4, &values[i]) != FDT_OK)
      return 0;
  }
  return 1;
}

static void
record_device_property(struct device_property_set *properties,
                       const struct fdt_event *event)
{
  uint32 *value = 0;
  int *present = 0;

  if (same_name(event->name, event->name_size, "compatible")) {
    properties->compatible = device_compatible(event);
    return;
  }
  if (same_name(event->name, event->name_size, "reg")) {
    properties->reg_property =
      read_cell(event, &properties->reg_value) ? 1 :
      read_device_ranges(event, properties) ? 2 : -1;
    return;
  }
  if (same_name(event->name, event->name_size, "interrupts")) {
    properties->interrupts_property = read_device_cells(
      event, properties->interrupts, &properties->interrupt_count) ? 1 : -1;
    return;
  }
  if (same_name(event->name, event->name_size, "interrupt-affinity")) {
    properties->affinity_property = read_device_cells(
      event, properties->affinities, &properties->affinity_count) ? 1 : -1;
    return;
  }
  if (same_name(event->name, event->name_size, "ranges")) {
    properties->ranges_property = event->value_size == 0 ? 1 : -1;
    return;
  }
  if (same_name(event->name, event->name_size, "interrupt-controller")) {
    properties->interrupt_controller = event->value_size == 0 ? 1 : -1;
    return;
  }
  if (same_name(event->name, event->name_size, "serial0")) {
    properties->serial0 = copy_string(
      event, properties->serial0_value, DEVICE_PATH_SIZE) ? 1 : -1;
    return;
  }
  if (same_name(event->name, event->name_size, "stdout-path")) {
    properties->stdout_path = copy_string(
      event, properties->stdout_path_value, DEVICE_PATH_SIZE) ? 1 : -1;
    return;
  }

#define DEVICE_CELL(prop_name, flag, field)                                \
  if (same_name(event->name, event->name_size, (prop_name))) {             \
    present = &properties->flag;                                           \
    value = &properties->field;                                            \
  } else
  DEVICE_CELL("#address-cells", address_cells, address_cells_value)
  DEVICE_CELL("#size-cells", size_cells, size_cells_value)
  DEVICE_CELL("interrupt-parent", interrupt_parent, interrupt_parent_value)
  DEVICE_CELL("phandle", phandle, phandle_value)
  DEVICE_CELL("linux,phandle", linux_phandle, linux_phandle_value)
  DEVICE_CELL("#interrupt-cells", interrupt_cells, interrupt_cells_value)
  DEVICE_CELL("qemu,source-count", source_count, source_count_value)
  DEVICE_CELL("qemu,context-count", context_count, context_count_value)
  DEVICE_CELL("qemu,context-stride", context_stride, context_stride_value)
  DEVICE_CELL("qemu,request-bit", request_bit, request_bit_value)
  DEVICE_CELL("clock-frequency", clock_frequency, clock_frequency_value)
  DEVICE_CELL("current-speed", current_speed, current_speed_value)
  DEVICE_CELL("reg-shift", register_shift, register_shift_value)
  DEVICE_CELL("reg-io-width", register_width, register_width_value)
  DEVICE_CELL("memory-region", memory_region, memory_region_value)
  {
  }
#undef DEVICE_CELL
  if (present != 0)
    *present = read_cell(event, value) ? 1 : -1;
}

static enum device_role
device_role(const struct device_decoder *decoder,
            const struct fdt_event *event)
{
  if (event->depth == 1) {
    if (same_name(event->name, event->name_size, "soc"))
      return DEVICE_SOC;
    if (same_name(event->name, event->name_size, "aliases"))
      return DEVICE_ALIASES;
    if (same_name(event->name, event->name_size, "chosen"))
      return DEVICE_CHOSEN;
    if (same_name(event->name, event->name_size, "cpus"))
      return DEVICE_CPUS;
    if (same_name(event->name, event->name_size, "reserved-memory"))
      return DEVICE_RESERVED_MEMORY;
  }
  if (event->depth == 2 && decoder->roles[1] == DEVICE_SOC)
    return DEVICE_SOC_CHILD;
  if (event->depth == 2 && decoder->roles[1] == DEVICE_CPUS)
    return DEVICE_CPU;
  if (event->depth == 2 && decoder->roles[1] == DEVICE_RESERVED_MEMORY)
    return DEVICE_RESERVED_CHILD;
  return DEVICE_OTHER;
}

static int
close_device_node(struct device_decoder *decoder, uint32 depth)
{
  struct device_property_set *properties = &decoder->properties[depth];

  switch (decoder->roles[depth]) {
  case DEVICE_SOC:
    decoder->soc_count++;
    if (properties->compatible != DEVICE_KIND_SOC ||
        properties->address_cells != 1 ||
        properties->address_cells_value != 2 || properties->size_cells != 1 ||
        properties->size_cells_value != 2 || properties->ranges_property != 1)
      return PLATFORM_BAD_SOC;
    break;
  case DEVICE_ALIASES:
    decoder->aliases_count++;
    if (properties->serial0 != 1)
      return PLATFORM_BAD_UART;
    for (uint32 i = 0; i < DEVICE_PATH_SIZE; i++)
      decoder->serial0[i] = properties->serial0_value[i];
    break;
  case DEVICE_CHOSEN:
    decoder->chosen_count++;
    if (properties->stdout_path != 1)
      return PLATFORM_BAD_UART;
    for (uint32 i = 0; i < DEVICE_PATH_SIZE; i++)
      decoder->stdout_path[i] = properties->stdout_path_value[i];
    break;
  case DEVICE_CPU:
    if (decoder->cpu_count == NCPU || properties->reg_property != 1 ||
        properties->phandle != 1 || properties->linux_phandle != 1 ||
        properties->phandle_value == 0 ||
        properties->phandle_value != properties->linux_phandle_value)
      return PLATFORM_BAD_DEVICE_REFERENCE;
    decoder->cpus[decoder->cpu_count++] = (struct device_cpu_handle) {
      .id = properties->reg_value,
      .phandle = properties->phandle_value,
    };
    break;
  case DEVICE_RESERVED_CHILD:
    if (properties->compatible == DEVICE_KIND_FRAMEBUFFER_MEMORY) {
      decoder->framebuffer_memory_count++;
      if (properties->phandle != 1 || properties->linux_phandle != 1 ||
          properties->phandle_value == 0 ||
          properties->phandle_value != properties->linux_phandle_value)
        return PLATFORM_BAD_DEVICE_REFERENCE;
      decoder->framebuffer_memory_phandle = properties->phandle_value;
    }
    break;
  case DEVICE_SOC_CHILD:
    switch (properties->compatible) {
    case DEVICE_KIND_INTC:
      decoder->intc_count++;
      decoder->intc = *properties;
      break;
    case DEVICE_KIND_UART:
      decoder->uart_count++;
      decoder->uart = *properties;
      break;
    case DEVICE_KIND_TIMER:
      decoder->timer_count++;
      decoder->timer = *properties;
      break;
    case DEVICE_KIND_IPI:
      decoder->ipi_count++;
      decoder->ipi = *properties;
      break;
    case DEVICE_KIND_FRAMEBUFFER:
      decoder->framebuffer_count++;
      decoder->framebuffer = *properties;
      break;
    case DEVICE_KIND_VIRTIO:
      if (decoder->virtio_count == PLATFORM_VIRTIO_SLOTS)
        return PLATFORM_BAD_VIRTIO;
      decoder->virtio[decoder->virtio_count++] = (struct raw_virtio) {
        .registers = properties->ranges[0],
        .reg_property = properties->reg_property,
        .range_count = properties->range_count,
        .interrupts_property = properties->interrupts_property,
        .interrupt_count = properties->interrupt_count,
        .interrupt = properties->interrupts[0],
        .interrupt_parent = properties->interrupt_parent,
        .interrupt_parent_value = properties->interrupt_parent_value,
      };
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
  return PLATFORM_OK;
}

static int
decode_device_nodes(const struct fdt *fdt, struct device_decoder *decoder)
{
  struct fdt_iterator iterator;
  struct fdt_event event;
  int status;

  fdt_iterator_init(fdt, &iterator);
  for (;;) {
    status = fdt_next(fdt, &iterator, &event);
    if (status != FDT_OK)
      return PLATFORM_BAD_ARGUMENT;
    if (event.type == FDT_EVENT_BEGIN_NODE) {
      if (event.depth < PLATFORM_DECODE_DEPTHS) {
        decoder->roles[event.depth] = device_role(decoder, &event);
        decoder->properties[event.depth] =
          (struct device_property_set) { 0 };
        decoder->properties[event.depth].node_name_valid =
          copy_node_name(&event,
                         decoder->properties[event.depth].node_name,
                         DEVICE_PATH_SIZE);
      }
    } else if (event.type == FDT_EVENT_PROPERTY) {
      if (event.depth < PLATFORM_DECODE_DEPTHS)
        record_device_property(&decoder->properties[event.depth], &event);
    } else if (event.type == FDT_EVENT_END_NODE) {
      if (event.depth < PLATFORM_DECODE_DEPTHS) {
        status = close_device_node(decoder, event.depth);
        if (status != PLATFORM_OK)
          return status;
      }
    } else {
      return PLATFORM_OK;
    }
  }
}

static int
range_is(const struct platform_mmio_range *range, uint64 start, uint64 size)
{
  return range->start == start && range->size == size;
}

static int
parent_is(const struct device_property_set *device, uint32 phandle)
{
  return device->interrupt_parent == 1 &&
         device->interrupt_parent_value == phandle;
}

static int
path_is_uart(const char *path, const char *node_name)
{
  const char prefix[] = "/soc/";
  uint32 offset = 0;

  while (prefix[offset] != 0) {
    if (path[offset] != prefix[offset])
      return 0;
    offset++;
  }
  for (uint32 i = 0; offset + i < DEVICE_PATH_SIZE; i++) {
    if (path[offset + i] != node_name[i])
      return 0;
    if (node_name[i] == 0)
      return 1;
  }
  return 0;
}

static int
c_string_is(const char *value, const char *expected)
{
  uint32 size = 0;

  while (size < DEVICE_PATH_SIZE && value[size] != 0)
    size++;
  return size < DEVICE_PATH_SIZE &&
         same_name((const uint8 *)value, size, expected);
}

static int
cpu_for_phandle(const struct device_decoder *decoder, uint32 phandle,
                uint32 *cpu_id)
{
  int found = 0;

  for (uint32 i = 0; i < decoder->cpu_count; i++) {
    if (decoder->cpus[i].phandle == phandle) {
      if (found)
        return 0;
      *cpu_id = decoder->cpus[i].id;
      found = 1;
    }
  }
  return found;
}

static int
normalize_devices(const struct fdt *fdt, const struct device_decoder *decoder,
                  const struct platform *platform,
                  struct platform_devices *devices)
{
  const struct device_property_set *intc = &decoder->intc;
  const struct device_property_set *uart = &decoder->uart;
  const struct device_property_set *timer = &decoder->timer;
  const struct device_property_set *ipi = &decoder->ipi;
  const struct device_property_set *framebuffer = &decoder->framebuffer;
  uint32 cpus = platform->topology.count;
  uint32 intc_phandle;
  int occupied[PLATFORM_VIRTIO_SLOTS] = { 0 };

  if (decoder->soc_count != 1)
    return PLATFORM_BAD_SOC;
  if (decoder->aliases_count != 1 || decoder->chosen_count != 1 ||
      decoder->uart_count != 1)
    return PLATFORM_BAD_UART;
  if (decoder->cpu_count != cpus || decoder->framebuffer_memory_count != 1)
    return PLATFORM_BAD_DEVICE_REFERENCE;
  if (decoder->intc_count != 1 || intc->reg_property != 2 ||
      intc->range_count != 2 ||
      !range_is(&intc->ranges[0], REQUIRED_INTC_BASE, 0x10000) ||
      !range_is(&intc->ranges[1], REQUIRED_INTC_CONTEXT_BASE,
                (uint64)cpus * REQUIRED_CONTEXT_STRIDE) ||
      intc->interrupt_controller != 1 || intc->interrupt_cells != 1 ||
      intc->interrupt_cells_value != 1 || intc->source_count != 1 ||
      intc->source_count_value != REQUIRED_INTC_SOURCES ||
      intc->context_count != 1 || intc->context_count_value != cpus ||
      intc->context_stride != 1 ||
      intc->context_stride_value != REQUIRED_CONTEXT_STRIDE ||
      intc->phandle != 1 || intc->linux_phandle != 1 ||
      intc->phandle_value == 0 ||
      intc->phandle_value != intc->linux_phandle_value ||
      phandle_is_unique(fdt, intc->phandle_value) != PLATFORM_OK)
    return PLATFORM_BAD_INTERRUPT_CONTROLLER;
  intc_phandle = intc->phandle_value;

  if (uart->reg_property != 2 || uart->range_count != 1 ||
      !range_is(&uart->ranges[0], REQUIRED_UART_BASE, 8) ||
      uart->interrupts_property != 1 || uart->interrupt_count != 1 ||
      uart->interrupts[0] != 1 || !parent_is(uart, intc_phandle) ||
      uart->clock_frequency != 1 || uart->clock_frequency_value != 1843200 ||
      uart->current_speed != 1 || uart->current_speed_value != 115200 ||
      uart->register_shift != 1 || uart->register_shift_value != 0 ||
      uart->register_width != 1 || uart->register_width_value != 1 ||
      !uart->node_name_valid ||
      !path_is_uart(decoder->serial0, uart->node_name) ||
      !c_string_is(decoder->stdout_path, "serial0:115200n8"))
    return PLATFORM_BAD_UART;

  if (decoder->timer_count != 1 || timer->reg_property != 2 ||
      timer->range_count != 2 ||
      !range_is(&timer->ranges[0], REQUIRED_TIMER_BASE, 0x10000) ||
      !range_is(&timer->ranges[1], REQUIRED_TIMER_CONTEXT_BASE,
                (uint64)cpus * REQUIRED_CONTEXT_STRIDE) ||
      timer->interrupts_property != 1 || timer->interrupt_count != cpus ||
      timer->affinity_property != 1 || timer->affinity_count != cpus ||
      !parent_is(timer, intc_phandle) || timer->context_count != 1 ||
      timer->context_count_value != cpus || timer->context_stride != 1 ||
      timer->context_stride_value != REQUIRED_CONTEXT_STRIDE ||
      timer->clock_frequency != 1 ||
      timer->clock_frequency_value != 1000000000U)
    return PLATFORM_BAD_TIMER;
  for (uint32 i = 0; i < cpus; i++) {
    uint32 cpu_id;

    if (timer->interrupts[i] != REQUIRED_TIMER_SOURCE_BASE + i ||
        !cpu_for_phandle(decoder, timer->affinities[i], &cpu_id) ||
        cpu_id != i)
      return PLATFORM_BAD_TIMER;
  }

  if (decoder->ipi_count != 1 || ipi->reg_property != 2 ||
      ipi->range_count != 2 ||
      !range_is(&ipi->ranges[0], REQUIRED_IPI_BASE, 0x10000) ||
      !range_is(&ipi->ranges[1], REQUIRED_IPI_CONTEXT_BASE,
                (uint64)cpus * REQUIRED_CONTEXT_STRIDE) ||
      ipi->context_count != 1 || ipi->context_count_value != cpus ||
      ipi->context_stride != 1 ||
      ipi->context_stride_value != REQUIRED_CONTEXT_STRIDE ||
      ipi->request_bit != 1 || ipi->request_bit_value != 9)
    return PLATFORM_BAD_IPI;

  if (decoder->framebuffer_count != 1 || framebuffer->reg_property != 2 ||
      framebuffer->range_count != 1 ||
      !range_is(&framebuffer->ranges[0], REQUIRED_FRAMEBUFFER_BASE, 0x1000) ||
      framebuffer->memory_region != 1 ||
      framebuffer->memory_region_value != decoder->framebuffer_memory_phandle)
    return PLATFORM_BAD_FRAMEBUFFER_DEVICE;

  if (decoder->virtio_count != PLATFORM_VIRTIO_SLOTS)
    return PLATFORM_BAD_VIRTIO;
  *devices = (struct platform_devices) { 0 };
  for (uint32 i = 0; i < decoder->virtio_count; i++) {
    const struct raw_virtio *virtio = &decoder->virtio[i];
    uint64 offset;
    uint32 slot;

    if (virtio->reg_property != 2 || virtio->range_count != 1 ||
        virtio->registers.start < REQUIRED_VIRTIO_BASE ||
        virtio->registers.size != 0x200 ||
        virtio->interrupt_parent != 1 ||
        virtio->interrupt_parent_value != intc_phandle)
      return PLATFORM_BAD_VIRTIO;
    offset = virtio->registers.start - REQUIRED_VIRTIO_BASE;
    if (offset % REQUIRED_CONTEXT_STRIDE != 0 ||
        offset / REQUIRED_CONTEXT_STRIDE >= PLATFORM_VIRTIO_SLOTS)
      return PLATFORM_BAD_VIRTIO;
    slot = offset / REQUIRED_CONTEXT_STRIDE;
    if (occupied[slot] || virtio->interrupts_property != 1 ||
        virtio->interrupt_count != 1 ||
        virtio->interrupt != REQUIRED_VIRTIO_SOURCE_BASE + slot)
      return PLATFORM_BAD_VIRTIO;
    occupied[slot] = 1;
    devices->virtio[slot] = (struct platform_virtio_slot) {
      .registers = virtio->registers,
      .interrupt = virtio->interrupt,
    };
  }

  devices->interrupt_controller = (struct platform_interrupt_controller) {
    .global = intc->ranges[0],
    .contexts = intc->ranges[1],
    .source_count = intc->source_count_value,
    .context_count = intc->context_count_value,
    .context_stride = intc->context_stride_value,
  };
  devices->uart = (struct platform_uart) {
    .registers = uart->ranges[0],
    .interrupt = uart->interrupts[0],
    .clock_frequency = uart->clock_frequency_value,
    .baud_rate = uart->current_speed_value,
    .register_shift = uart->register_shift_value,
    .register_width = uart->register_width_value,
  };
  devices->timer = (struct platform_timer) {
    .global = timer->ranges[0],
    .contexts = timer->ranges[1],
    .context_count = timer->context_count_value,
    .context_stride = timer->context_stride_value,
    .clock_frequency = timer->clock_frequency_value,
  };
  for (uint32 i = 0; i < cpus; i++)
    devices->timer.interrupts[i] = timer->interrupts[i];
  devices->ipi = (struct platform_ipi) {
    .global = ipi->ranges[0],
    .contexts = ipi->ranges[1],
    .context_count = ipi->context_count_value,
    .context_stride = ipi->context_stride_value,
    .request_bit = ipi->request_bit_value,
  };
  devices->virtio_count = PLATFORM_VIRTIO_SLOTS;
  devices->framebuffer.control = framebuffer->ranges[0];
  for (uint32 i = 0; i < platform->memory.reservation_count; i++) {
    const struct platform_reservation *reservation =
      &platform->memory.reservations[i];

    if (reservation->owner == PLATFORM_RESERVATION_FRAMEBUFFER) {
      devices->framebuffer.memory = (struct platform_mmio_range) {
        .start = reservation->start,
        .size = reservation->size,
      };
      return PLATFORM_OK;
    }
  }
  return PLATFORM_BAD_FRAMEBUFFER_DEVICE;
}

int
platform_decode_cpu_topology(const struct fdt *fdt,
                             struct platform_cpu_topology *topology)
{
  struct topology_decoder decoder = { 0 };
  int status;

  if (fdt == 0 || topology == 0)
    return PLATFORM_BAD_ARGUMENT;
  status = decode_nodes(fdt, &decoder);
  if (status != PLATFORM_OK)
    return status;
  return normalize_topology(fdt, &decoder, topology);
}

int
platform_decode(const struct fdt *fdt, uint64 fdt_address,
                struct platform *platform)
{
  struct topology_decoder decoder = { 0 };
  struct platform result = { 0 };
  int status;

  if (fdt == 0 || platform == 0)
    return PLATFORM_BAD_ARGUMENT;
  status = decode_nodes(fdt, &decoder);
  if (status != PLATFORM_OK)
    return status;
  status = normalize_topology(fdt, &decoder, &result.topology);
  if (status != PLATFORM_OK)
    return status;
  status = normalize_memory(fdt, fdt_address, &decoder, &result.topology,
                            &result.memory);
  if (status != PLATFORM_OK)
    return status;
  *platform = result;
  return PLATFORM_OK;
}

int
platform_decode_devices(const struct fdt *fdt, struct platform *platform)
{
  struct device_decoder *decoder = &device_scratch;
  struct platform_devices devices;
  int status;

  if (fdt == 0 || platform == 0 || platform->topology.count == 0 ||
      platform->topology.count > NCPU)
    return PLATFORM_BAD_ARGUMENT;
  *decoder = (struct device_decoder) { 0 };
  status = decode_device_nodes(fdt, decoder);
  if (status != PLATFORM_OK)
    return status;
  status = normalize_devices(fdt, decoder, platform, &devices);
  if (status != PLATFORM_OK)
    return status;
  platform->devices = devices;
  return PLATFORM_OK;
}

static void
copy_physical_range(struct platform_physical_range *destination,
                    const struct platform_mmio_range *source)
{
  destination->physical_base = source->start;
  destination->size = source->size;
}

uint64
platform_fdt_physical_address(void)
{
  return mmix_fdt_address;
}

uint32
platform_cpu_count(void)
{
  return mmix_platform.topology.count;
}

uint64
platform_cpu_mask(void)
{
  uint32 count = platform_cpu_count();

  if (count == 0 || count > NCPU)
    return 0;
  return (1ULL << count) - 1;
}

int
platform_cpu_initial_stack(uint32 cpu_id, struct platform_physical_range *stack)
{
  const struct platform_cpu *cpu;

  if (stack == 0 || cpu_id >= platform_cpu_count())
    return PLATFORM_BAD_ARGUMENT;
  cpu = &mmix_platform.topology.cpus[cpu_id];
  *stack = (struct platform_physical_range){
    .physical_base = cpu->initial_register_stack,
    .size = cpu->initial_register_stack_size,
  };
  return PLATFORM_OK;
}

int
platform_cpu_initial_stack_contains(uint32 cpu_id, uint64 physical_address)
{
  struct platform_physical_range stack;

  if (platform_cpu_initial_stack(cpu_id, &stack) != PLATFORM_OK)
    return 0;
  return physical_address >= stack.physical_base &&
         physical_address - stack.physical_base < stack.size;
}

int
platform_ram(struct platform_physical_range *ram)
{
  if (ram == 0)
    return PLATFORM_BAD_ARGUMENT;
  *ram = (struct platform_physical_range){
    .physical_base = mmix_platform.memory.ram_start,
    .size = mmix_platform.memory.ram_size,
  };
  return PLATFORM_OK;
}

uint32
platform_reservation_count(void)
{
  return mmix_platform.memory.reservation_count;
}

int
platform_reservation(uint32 index, struct platform_reservation_info *info)
{
  const struct platform_reservation *reservation;

  if (info == 0 || index >= platform_reservation_count())
    return PLATFORM_BAD_ARGUMENT;
  reservation = &mmix_platform.memory.reservations[index];
  *info = (struct platform_reservation_info){
    .physical =
      {
        .physical_base = reservation->start,
        .size = reservation->size,
      },
    .owner = reservation->owner,
    .lifetime = reservation->lifetime,
    .cpu_id = reservation->cpu_id,
  };
  return PLATFORM_OK;
}

int
platform_intc_config(struct platform_intc_config *config)
{
  const struct platform_interrupt_controller *intc;

  if (config == 0)
    return PLATFORM_BAD_ARGUMENT;
  intc = &mmix_platform.devices.interrupt_controller;
  copy_physical_range(&config->physical_global, &intc->global);
  copy_physical_range(&config->physical_contexts, &intc->contexts);
  config->source_count = intc->source_count;
  config->context_count = intc->context_count;
  config->context_stride = intc->context_stride;
  return PLATFORM_OK;
}

int
platform_uart_config(struct platform_uart_config *config)
{
  const struct platform_uart *uart;

  if (config == 0)
    return PLATFORM_BAD_ARGUMENT;
  uart = &mmix_platform.devices.uart;
  copy_physical_range(&config->physical_registers, &uart->registers);
  config->interrupt = uart->interrupt;
  config->clock_frequency = uart->clock_frequency;
  config->baud_rate = uart->baud_rate;
  config->register_shift = uart->register_shift;
  config->register_width = uart->register_width;
  return PLATFORM_OK;
}

int
platform_timer_config(struct platform_timer_config *config)
{
  const struct platform_timer *timer;

  if (config == 0)
    return PLATFORM_BAD_ARGUMENT;
  timer = &mmix_platform.devices.timer;
  copy_physical_range(&config->physical_global, &timer->global);
  copy_physical_range(&config->physical_contexts, &timer->contexts);
  config->context_count = timer->context_count;
  config->context_stride = timer->context_stride;
  config->clock_frequency = timer->clock_frequency;
  return PLATFORM_OK;
}

int
platform_timer_interrupt(uint32 cpu_id, uint32 *interrupt)
{
  if (interrupt == 0 || cpu_id >= platform_cpu_count() ||
      cpu_id >= mmix_platform.devices.timer.context_count)
    return PLATFORM_BAD_ARGUMENT;
  *interrupt = mmix_platform.devices.timer.interrupts[cpu_id];
  return PLATFORM_OK;
}

int
platform_ipi_config(struct platform_ipi_config *config)
{
  const struct platform_ipi *ipi;

  if (config == 0)
    return PLATFORM_BAD_ARGUMENT;
  ipi = &mmix_platform.devices.ipi;
  copy_physical_range(&config->physical_global, &ipi->global);
  copy_physical_range(&config->physical_contexts, &ipi->contexts);
  config->context_count = ipi->context_count;
  config->context_stride = ipi->context_stride;
  config->request_bit = ipi->request_bit;
  return PLATFORM_OK;
}

uint32
platform_virtio_count(void)
{
  return mmix_platform.devices.virtio_count;
}

int
platform_virtio_config(uint32 slot, struct platform_virtio_config *config)
{
  const struct platform_virtio_slot *virtio;

  if (config == 0 || slot >= platform_virtio_count())
    return PLATFORM_BAD_ARGUMENT;
  virtio = &mmix_platform.devices.virtio[slot];
  copy_physical_range(&config->physical_registers, &virtio->registers);
  config->interrupt = virtio->interrupt;
  return PLATFORM_OK;
}

int
platform_framebuffer_config(struct platform_framebuffer_config *config)
{
  const struct platform_framebuffer *framebuffer;

  if (config == 0)
    return PLATFORM_BAD_ARGUMENT;
  framebuffer = &mmix_platform.devices.framebuffer;
  copy_physical_range(&config->physical_control, &framebuffer->control);
  copy_physical_range(&config->physical_memory, &framebuffer->memory);
  return PLATFORM_OK;
}

_Static_assert(NCPU == 16, "platform topology must match the xv6 CPU limit");
_Static_assert((INITIAL_REGISTER_STACK_SIZE & (MMIX_PAGE_SIZE - 1)) == 0,
               "initial register stacks must contain whole pages");
_Static_assert(PLATFORM_RAM_MIN_SIZE == 0x08000000UL,
               "platform RAM minimum must be 128 MiB");
_Static_assert(PLATFORM_RAM_MAX_SIZE == 0x40000000UL,
               "platform RAM maximum must be 1 GiB");
_Static_assert(PLATFORM_MAX_RESERVATIONS == NCPU + 2,
               "platform reservations need FDT, CPU stacks, and framebuffer");
_Static_assert(PLATFORM_VIRTIO_SLOTS == 32,
               "platform must describe every VirtIO MMIO slot");
