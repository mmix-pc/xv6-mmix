#ifndef XV6_MMIX_VIRTIO_H
#define XV6_MMIX_VIRTIO_H

#include "types.h"

// VirtIO MMIO transport and split-ring definitions.
// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html

// Register offsets from the version 2 MMIO transport specification.
#define VIRTIO_MMIO_MAGIC_VALUE         0x000 // read-only; 0x74726976
#define VIRTIO_MMIO_VERSION             0x004 // read-only; must be 2
#define VIRTIO_MMIO_DEVICE_ID           0x008 // read-only; 2 is a block device
#define VIRTIO_MMIO_VENDOR_ID           0x00c // read-only; QEMU is 0x554d4551
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010 // read-only selected feature word
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014 // write-only feature-word selector
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020 // write-only selected feature word
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024 // write-only feature-word selector
#define VIRTIO_MMIO_QUEUE_SEL           0x030 // write-only current queue selector
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034 // read-only maximum queue size
#define VIRTIO_MMIO_QUEUE_NUM           0x038 // write-only selected queue size
#define VIRTIO_MMIO_QUEUE_READY         0x044 // read/write selected ready bit
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050 // write-only queue number
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060 // read-only interrupt status
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064 // write-only interrupt ack
#define VIRTIO_MMIO_STATUS              0x070 // read/write device status
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080 // write descriptor address bits 31:0
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084 // write descriptor address bits 63:32
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW    0x090 // write available address bits 31:0
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH   0x094 // write available address bits 63:32
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW    0x0a0 // write used address bits 31:0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH   0x0a4 // write used address bits 63:32
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc // read-only configuration version
#define VIRTIO_MMIO_CONFIG              0x100 // device-specific config begins here

// Device status bits.
#define VIRTIO_CONFIG_S_ACKNOWLEDGE       1
#define VIRTIO_CONFIG_S_DRIVER            2
#define VIRTIO_CONFIG_S_DRIVER_OK         4
#define VIRTIO_CONFIG_S_FEATURES_OK       8
#define VIRTIO_CONFIG_S_DEVICE_NEEDS_RESET 64
#define VIRTIO_CONFIG_S_FAILED            128

// Device feature bit numbers.
#define VIRTIO_BLK_F_RO 5     // device is read-only
#define VIRTIO_F_VERSION_1 32 // device complies with VirtIO 1.0 or later
#define VIRTIO_FEATURE_WORD(bit) ((bit) / 32)
#define VIRTIO_FEATURE_MASK(bit) (1U << ((bit) % 32))

#define VIRTIO_BLK_T_IN  0 // read from the disk
#define VIRTIO_BLK_T_OUT 1 // write to the disk

#define VRING_DESC_F_NEXT  1 // chained with another descriptor
#define VRING_DESC_F_WRITE 2 // device writes into the described buffer

// The initial split ring has eight descriptors, as in common xv6.
#define NUM 8
#define VIRTQ_DESC_ALIGN  16 // descriptor-table alignment
#define VIRTQ_AVAIL_ALIGN 2  // available-ring alignment
#define VIRTQ_USED_ALIGN  4  // used-ring alignment

// Multi-byte device-owned fields are byte arrays so native big-endian C
// accesses cannot accidentally reinterpret their little-endian encoding.
struct virtq_desc {
  uint8 addr[8];
  uint8 len[4];
  uint8 flags[2];
  uint8 next[2];
} __attribute__((aligned(VIRTQ_DESC_ALIGN)));

struct virtq_avail {
  uint8 flags[2];
  uint8 idx[2];        // driver advances after publishing an entry
  uint8 ring[NUM][2];  // descriptor-chain head indices
  uint8 used_event[2]; // reserved because EVENT_IDX is not negotiated
};

// One device-produced completion entry, identified by descriptor-chain head.
struct virtq_used_elem {
  uint8 id[4];
  uint8 len[4];
};

struct virtq_used {
  uint8 flags[2];
  uint8 idx[2]; // device advances after publishing a completion
  struct virtq_used_elem ring[NUM];
  uint8 avail_event[2]; // reserved because EVENT_IDX is not negotiated
};

// The first descriptor of a block request. The next two descriptors contain
// the data buffer and the one-byte device status, respectively.
struct virtio_blk_req {
  uint8 type[4];
  uint8 reserved[4];
  uint8 sector[8];
} __attribute__((aligned(8)));

static inline uint16
virtio_load_le16(const uint8 field[2])
{
  return (uint16)field[0] | (uint16)field[1] << 8;
}

static inline uint32
virtio_load_le32(const uint8 field[4])
{
  return (uint32)field[0] | (uint32)field[1] << 8 |
         (uint32)field[2] << 16 | (uint32)field[3] << 24;
}

static inline uint64
virtio_load_le64(const uint8 field[8])
{
  return (uint64)virtio_load_le32(field) |
         (uint64)virtio_load_le32(field + 4) << 32;
}

static inline void
virtio_store_le16(uint8 field[2], uint16 value)
{
  field[0] = value;
  field[1] = value >> 8;
}

static inline void
virtio_store_le32(uint8 field[4], uint32 value)
{
  field[0] = value;
  field[1] = value >> 8;
  field[2] = value >> 16;
  field[3] = value >> 24;
}

static inline void
virtio_store_le64(uint8 field[8], uint64 value)
{
  virtio_store_le32(field, (uint32)value);
  virtio_store_le32(field + 4, (uint32)(value >> 32));
}

_Static_assert(NUM == 8 && (NUM & (NUM - 1)) == 0,
               "VirtIO queue size must remain a power of two");
_Static_assert(sizeof(struct virtq_desc) == 16 &&
                 __alignof__(struct virtq_desc) == VIRTQ_DESC_ALIGN &&
                 __builtin_offsetof(struct virtq_desc, addr) == 0 &&
                 __builtin_offsetof(struct virtq_desc, len) == 8 &&
                 __builtin_offsetof(struct virtq_desc, flags) == 12 &&
                 __builtin_offsetof(struct virtq_desc, next) == 14,
               "VirtIO descriptor wire layout mismatch");
_Static_assert(sizeof(struct virtq_avail) == 22 &&
                 __builtin_offsetof(struct virtq_avail, flags) == 0 &&
                 __builtin_offsetof(struct virtq_avail, idx) == 2 &&
                 __builtin_offsetof(struct virtq_avail, ring) == 4 &&
                 __builtin_offsetof(struct virtq_avail, used_event) == 20,
               "VirtIO available-ring wire layout mismatch");
_Static_assert(sizeof(struct virtq_used_elem) == 8 &&
                 __builtin_offsetof(struct virtq_used_elem, id) == 0 &&
                 __builtin_offsetof(struct virtq_used_elem, len) == 4 &&
                 sizeof(struct virtq_used) == 70 &&
                 __builtin_offsetof(struct virtq_used, flags) == 0 &&
                 __builtin_offsetof(struct virtq_used, idx) == 2 &&
                 __builtin_offsetof(struct virtq_used, ring) == 4 &&
                 __builtin_offsetof(struct virtq_used, avail_event) == 68,
               "VirtIO used-ring wire layout mismatch");
_Static_assert(sizeof(struct virtio_blk_req) == 16 &&
                 __alignof__(struct virtio_blk_req) == 8 &&
                 __builtin_offsetof(struct virtio_blk_req, type) == 0 &&
                 __builtin_offsetof(struct virtio_blk_req, reserved) == 4 &&
                 __builtin_offsetof(struct virtio_blk_req, sector) == 8,
               "VirtIO block-request wire layout mismatch");
_Static_assert((VIRTQ_DESC_ALIGN & (VIRTQ_DESC_ALIGN - 1)) == 0 &&
                 (VIRTQ_AVAIL_ALIGN & (VIRTQ_AVAIL_ALIGN - 1)) == 0 &&
                 (VIRTQ_USED_ALIGN & (VIRTQ_USED_ALIGN - 1)) == 0,
               "VirtIO split-ring alignments must be powers of two");

#endif
