#include "types.h"
#include "fdt.h"
#include "memlayout.h"
#include "platform.h"

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

_Static_assert(NCPU == 16, "platform topology must match the xv6 CPU limit");
_Static_assert((INITIAL_REGISTER_STACK_SIZE & (MMIX_PAGE_SIZE - 1)) == 0,
               "initial register stacks must contain whole pages");
_Static_assert(PLATFORM_RAM_MIN_SIZE == 0x08000000UL,
               "platform RAM minimum must be 128 MiB");
_Static_assert(PLATFORM_RAM_MAX_SIZE == 0x40000000UL,
               "platform RAM maximum must be 1 GiB");
_Static_assert(PLATFORM_MAX_RESERVATIONS == NCPU + 2,
               "platform reservations need FDT, CPU stacks, and framebuffer");
