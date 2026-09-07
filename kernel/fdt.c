#include "types.h"
#include "fdt.h"

enum {
  FDT_HEADER_SIZE = 40,
  FDT_MAGIC = 0xd00dfeed,
  FDT_VERSION = 17,
  FDT_BEGIN_NODE = 1,
  FDT_END_NODE = 2,
  FDT_PROP = 3,
  FDT_NOP = 4,
  FDT_END = 9,
};

static uint32
align4(uint32 value)
{
  return (value + 3) & ~3U;
}

static int
range_valid(uint32 total, uint32 offset, uint32 size)
{
  return offset <= total && size <= total - offset;
}

static int
ranges_overlap(uint32 left, uint32 left_size, uint32 right,
               uint32 right_size)
{
  if (left < right)
    return left_size > right - left;
  return right_size > left - right;
}

int
fdt_read_u32(const uint8 *bytes, uint32 size, uint32 *value)
{
  if (bytes == 0 || value == 0 || size < 4)
    return FDT_BAD_ARGUMENT;
  *value = (uint32)bytes[0] << 24 | (uint32)bytes[1] << 16 |
           (uint32)bytes[2] << 8 | bytes[3];
  return FDT_OK;
}

int
fdt_read_u64(const uint8 *bytes, uint32 size, uint64 *value)
{
  uint32 high;
  uint32 low;

  if (bytes == 0 || value == 0 || size < 8)
    return FDT_BAD_ARGUMENT;
  if (fdt_read_u32(bytes, size, &high) != FDT_OK ||
      fdt_read_u32(bytes + 4, size - 4, &low) != FDT_OK)
    return FDT_BAD_ARGUMENT;
  *value = (uint64)high << 32 | low;
  return FDT_OK;
}

static uint32
read_u32(const uint8 *bytes)
{
  uint32 value;

  fdt_read_u32(bytes, 4, &value);
  return value;
}

static uint64
read_u64(const uint8 *bytes)
{
  uint64 value;

  fdt_read_u64(bytes, 8, &value);
  return value;
}

static int
name_valid(const uint8 *name, uint32 size, int root)
{
  if ((root && size != 0) || (!root && size == 0))
    return 0;
  for (uint32 i = 0; i < size; i++) {
    if (name[i] < 0x21 || name[i] > 0x7e || name[i] == '/')
      return 0;
  }
  return 1;
}

static int
structure_name(const struct fdt *fdt, uint32 offset, const uint8 **name,
               uint32 *size, uint32 *next)
{
  uint32 end = fdt->structure_offset + fdt->structure_size;
  uint32 current = offset;

  while (current < end && fdt->blob[current] != 0)
    current++;
  if (current == end || current - offset > ~3U)
    return FDT_BAD_STRUCTURE;
  *name = fdt->blob + offset;
  *size = current - offset;
  current = align4(current + 1);
  if (current > end)
    return FDT_BAD_STRUCTURE;
  *next = current;
  return FDT_OK;
}

static int
property_name(const struct fdt *fdt, uint32 name_offset,
              const uint8 **name, uint32 *size)
{
  uint32 current;
  uint32 end;

  if (name_offset >= fdt->strings_size)
    return FDT_BAD_STRING;
  current = fdt->strings_offset + name_offset;
  end = fdt->strings_offset + fdt->strings_size;
  *name = fdt->blob + current;
  while (current < end && fdt->blob[current] != 0)
    current++;
  if (current == end || current == fdt->strings_offset + name_offset)
    return FDT_BAD_STRING;
  *size = current - (fdt->strings_offset + name_offset);
  return name_valid(*name, *size, 0) ? FDT_OK : FDT_BAD_STRING;
}

static int
same_bytes(const uint8 *left, uint32 left_size, const uint8 *right,
           uint32 right_size)
{
  if (left_size != right_size)
    return 0;
  for (uint32 i = 0; i < left_size; i++) {
    if (left[i] != right[i])
      return 0;
  }
  return 1;
}

static int
duplicate_property(const struct fdt *fdt, uint32 node_offset,
                   uint32 property_offset, const uint8 *name,
                   uint32 name_size)
{
  uint32 offset = node_offset;
  uint32 nested = 0;

  while (offset < property_offset) {
    uint32 token_offset = offset;
    uint32 token;

    if (!range_valid(property_offset, offset, 4))
      return FDT_BAD_STRUCTURE;
    token = read_u32(fdt->blob + offset);
    offset += 4;
    if (token == FDT_BEGIN_NODE) {
      const uint8 *ignored_name;
      uint32 ignored_size;
      int status = structure_name(fdt, offset, &ignored_name, &ignored_size,
                                  &offset);

      if (status != FDT_OK)
        return status;
      nested++;
    } else if (token == FDT_END_NODE) {
      if (nested == 0)
        return FDT_BAD_STRUCTURE;
      nested--;
    } else if (token == FDT_PROP) {
      uint32 value_size;
      uint32 name_offset;
      const uint8 *previous;
      uint32 previous_size;
      int status;

      if (!range_valid(property_offset, offset, 8))
        return FDT_BAD_STRUCTURE;
      value_size = read_u32(fdt->blob + offset);
      name_offset = read_u32(fdt->blob + offset + 4);
      offset += 8;
      if (!range_valid(property_offset, offset, value_size))
        return FDT_BAD_STRUCTURE;
      if (nested == 0) {
        status = property_name(fdt, name_offset, &previous, &previous_size);
        if (status != FDT_OK)
          return status;
        if (same_bytes(name, name_size, previous, previous_size))
          return FDT_DUPLICATE_PROPERTY;
      }
      if (value_size > ~3U)
        return FDT_BAD_STRUCTURE;
      offset = align4(offset + value_size);
    } else if (token != FDT_NOP) {
      return FDT_BAD_STRUCTURE;
    }
    if (offset <= token_offset || offset > property_offset)
      return FDT_BAD_STRUCTURE;
  }
  return offset == property_offset ? FDT_OK : FDT_BAD_STRUCTURE;
}

static int
validate_structure(struct fdt *fdt)
{
  uint32 node_offset[FDT_MAX_DEPTH];
  uint32 offset = fdt->structure_offset;
  uint32 end = offset + fdt->structure_size;
  uint32 depth = 0;
  int saw_root = 0;
  int closed_root = 0;

  while (offset < end) {
    uint32 token_offset = offset;
    uint32 token;

    if (!range_valid(end, offset, 4))
      return FDT_BAD_STRUCTURE;
    token = read_u32(fdt->blob + offset);
    offset += 4;
    if (token == FDT_BEGIN_NODE) {
      const uint8 *name;
      uint32 name_size;
      int status;

      if (closed_root || depth >= FDT_MAX_DEPTH)
        return depth >= FDT_MAX_DEPTH ? FDT_TOO_DEEP : FDT_BAD_STRUCTURE;
      status = structure_name(fdt, offset, &name, &name_size, &offset);
      if (status != FDT_OK || !name_valid(name, name_size, depth == 0))
        return FDT_BAD_STRUCTURE;
      if (depth == 0) {
        if (saw_root)
          return FDT_BAD_STRUCTURE;
        saw_root = 1;
      }
      node_offset[depth++] = offset;
    } else if (token == FDT_END_NODE) {
      if (depth == 0)
        return FDT_BAD_STRUCTURE;
      depth--;
      if (depth == 0)
        closed_root = 1;
    } else if (token == FDT_PROP) {
      uint32 value_size;
      uint32 name_offset;
      const uint8 *name;
      uint32 name_size;
      int status;

      if (depth == 0 || !range_valid(end, offset, 8))
        return FDT_BAD_STRUCTURE;
      value_size = read_u32(fdt->blob + offset);
      name_offset = read_u32(fdt->blob + offset + 4);
      offset += 8;
      if (!range_valid(end, offset, value_size) || value_size > ~3U)
        return FDT_BAD_STRUCTURE;
      status = property_name(fdt, name_offset, &name, &name_size);
      if (status != FDT_OK)
        return status;
      status = duplicate_property(fdt, node_offset[depth - 1], token_offset,
                                  name, name_size);
      if (status != FDT_OK)
        return status;
      offset = align4(offset + value_size);
      if (offset > end)
        return FDT_BAD_STRUCTURE;
    } else if (token == FDT_NOP) {
      continue;
    } else if (token == FDT_END) {
      if (!saw_root || !closed_root || depth != 0 || offset != end)
        return FDT_BAD_STRUCTURE;
      return FDT_OK;
    } else {
      return FDT_BAD_STRUCTURE;
    }
  }
  return FDT_BAD_STRUCTURE;
}

int
fdt_open(struct fdt *fdt, const void *blob, uint64 available)
{
  const uint8 *bytes = blob;
  uint32 reservation_end;
  uint32 version;
  uint32 compatible_version;

  if (fdt == 0 || blob == 0)
    return FDT_BAD_ARGUMENT;
  if (available < FDT_HEADER_SIZE)
    return FDT_BAD_SIZE;
  if (read_u32(bytes) != FDT_MAGIC)
    return FDT_BAD_MAGIC;
  fdt->total_size = read_u32(bytes + 4);
  if (fdt->total_size < FDT_HEADER_SIZE || fdt->total_size > FDT_MAX_SIZE ||
      fdt->total_size > available ||
      (uint64)blob > ~(uint64)0 - fdt->total_size)
    return FDT_BAD_SIZE;
  fdt->structure_offset = read_u32(bytes + 8);
  fdt->strings_offset = read_u32(bytes + 12);
  fdt->reservation_offset = read_u32(bytes + 16);
  version = read_u32(bytes + 20);
  compatible_version = read_u32(bytes + 24);
  fdt->strings_size = read_u32(bytes + 32);
  fdt->structure_size = read_u32(bytes + 36);
  fdt->blob = bytes;
  fdt->reservation_count = 0;

  if (version != FDT_VERSION || compatible_version > FDT_VERSION)
    return FDT_BAD_VERSION;
  if ((fdt->structure_offset & 3) != 0 ||
      (fdt->strings_offset & 3) != 0 ||
      (fdt->reservation_offset & 7) != 0 ||
      (fdt->structure_size & 3) != 0)
    return FDT_BAD_ALIGNMENT;
  if (fdt->structure_offset < FDT_HEADER_SIZE ||
      fdt->strings_offset < FDT_HEADER_SIZE ||
      fdt->reservation_offset < FDT_HEADER_SIZE ||
      !range_valid(fdt->total_size, fdt->structure_offset,
                   fdt->structure_size) ||
      !range_valid(fdt->total_size, fdt->strings_offset,
                   fdt->strings_size))
    return FDT_BAD_LAYOUT;

  reservation_end = fdt->reservation_offset;
  for (;;) {
    uint64 address;
    uint64 size;

    if (!range_valid(fdt->total_size, reservation_end, 16))
      return FDT_BAD_RESERVATION;
    address = read_u64(bytes + reservation_end);
    size = read_u64(bytes + reservation_end + 8);
    reservation_end += 16;
    if (address == 0 && size == 0)
      break;
    if (size == 0 || size > ~address)
      return FDT_BAD_RESERVATION;
    fdt->reservation_count++;
  }
  if (ranges_overlap(fdt->structure_offset, fdt->structure_size,
                     fdt->strings_offset, fdt->strings_size) ||
      ranges_overlap(fdt->reservation_offset,
                     reservation_end - fdt->reservation_offset,
                     fdt->structure_offset, fdt->structure_size) ||
      ranges_overlap(fdt->reservation_offset,
                     reservation_end - fdt->reservation_offset,
                     fdt->strings_offset, fdt->strings_size))
    return FDT_BAD_LAYOUT;
  return validate_structure(fdt);
}

void
fdt_iterator_init(const struct fdt *fdt, struct fdt_iterator *iterator)
{
  if (iterator == 0)
    return;
  iterator->offset = fdt == 0 ? 0 : fdt->structure_offset;
  iterator->depth = 0;
  iterator->ended = fdt == 0;
}

int
fdt_next(const struct fdt *fdt, struct fdt_iterator *iterator,
         struct fdt_event *event)
{
  uint32 token;
  uint32 structure_end;

  if (fdt == 0 || iterator == 0 || event == 0 || iterator->ended ||
      iterator->offset < fdt->structure_offset ||
      iterator->offset >= fdt->structure_offset + fdt->structure_size)
    return FDT_BAD_ARGUMENT;
  structure_end = fdt->structure_offset + fdt->structure_size;
  event->name = 0;
  event->name_size = 0;
  event->value = 0;
  event->value_size = 0;
  do {
    if (iterator->offset > structure_end - 4)
      return FDT_BAD_STRUCTURE;
    token = read_u32(fdt->blob + iterator->offset);
    iterator->offset += 4;
  } while (token == FDT_NOP);
  if (token == FDT_BEGIN_NODE) {
    int status = structure_name(fdt, iterator->offset, &event->name,
                                &event->name_size, &iterator->offset);

    if (status != FDT_OK)
      return status;
    event->type = FDT_EVENT_BEGIN_NODE;
    event->depth = iterator->depth++;
  } else if (token == FDT_END_NODE) {
    if (iterator->depth == 0)
      return FDT_BAD_STRUCTURE;
    event->type = FDT_EVENT_END_NODE;
    event->depth = --iterator->depth;
  } else if (token == FDT_PROP) {
    uint32 name_offset;
    int status;

    if (iterator->depth == 0)
      return FDT_BAD_STRUCTURE;
    event->value_size = read_u32(fdt->blob + iterator->offset);
    name_offset = read_u32(fdt->blob + iterator->offset + 4);
    iterator->offset += 8;
    event->value = fdt->blob + iterator->offset;
    status = property_name(fdt, name_offset, &event->name,
                           &event->name_size);
    if (status != FDT_OK)
      return status;
    iterator->offset = align4(iterator->offset + event->value_size);
    event->type = FDT_EVENT_PROPERTY;
    event->depth = iterator->depth - 1;
  } else if (token == FDT_END) {
    event->type = FDT_EVENT_END;
    event->depth = 0;
    iterator->ended = 1;
  } else {
    return FDT_BAD_STRUCTURE;
  }
  return FDT_OK;
}

int
fdt_reservation(const struct fdt *fdt, uint32 index, uint64 *address,
                uint64 *size)
{
  uint32 offset;

  if (fdt == 0 || address == 0 || size == 0 ||
      index >= fdt->reservation_count)
    return FDT_BAD_ARGUMENT;
  offset = fdt->reservation_offset + index * 16;
  *address = read_u64(fdt->blob + offset);
  *size = read_u64(fdt->blob + offset + 8);
  return FDT_OK;
}

_Static_assert(FDT_MAX_SIZE >= FDT_HEADER_SIZE,
               "FDT size limit cannot hold its header");
_Static_assert(FDT_MAX_DEPTH > 0, "FDT traversal needs a root depth");
