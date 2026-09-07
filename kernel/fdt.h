#ifndef XV6_MMIX_FDT_H
#define XV6_MMIX_FDT_H

// Internal interface for early MMIX platform discovery.
#define FDT_MAX_SIZE  0x00200000U
#define FDT_MAX_DEPTH 32U

enum fdt_status {
  FDT_OK = 0,
  FDT_BAD_ARGUMENT = -1,
  FDT_BAD_MAGIC = -2,
  FDT_BAD_SIZE = -3,
  FDT_BAD_VERSION = -4,
  FDT_BAD_ALIGNMENT = -5,
  FDT_BAD_LAYOUT = -6,
  FDT_BAD_RESERVATION = -7,
  FDT_BAD_STRUCTURE = -8,
  FDT_BAD_STRING = -9,
  FDT_DUPLICATE_PROPERTY = -10,
  FDT_TOO_DEEP = -11,
};

enum fdt_event_type {
  FDT_EVENT_BEGIN_NODE,
  FDT_EVENT_END_NODE,
  FDT_EVENT_PROPERTY,
  FDT_EVENT_END,
};

struct fdt {
  // These pointers remain valid only while the validated blob is immutable.
  const uint8 *blob;
  uint32 total_size;
  uint32 structure_offset;
  uint32 structure_size;
  uint32 strings_offset;
  uint32 strings_size;
  uint32 reservation_offset;
  uint32 reservation_count;
};

struct fdt_iterator {
  uint32 offset;
  uint32 depth;
  int ended;
};

struct fdt_event {
  // Depth identifies the node opened, closed, or containing the property.
  enum fdt_event_type type;
  uint32 depth;
  const uint8 *name;
  uint32 name_size;
  const uint8 *value;
  uint32 value_size;
};

// available is the readable byte bound beginning at blob.
int fdt_open(struct fdt *, const void *, uint64 available);
void fdt_iterator_init(const struct fdt *, struct fdt_iterator *);
int fdt_next(const struct fdt *, struct fdt_iterator *, struct fdt_event *);
int fdt_reservation(const struct fdt *, uint32, uint64 *, uint64 *);
int fdt_read_u32(const uint8 *, uint32, uint32 *);
int fdt_read_u64(const uint8 *, uint32, uint64 *);

#endif
