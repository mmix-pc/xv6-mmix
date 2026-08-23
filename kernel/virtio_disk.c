// Modern VirtIO MMIO block driver using an interrupt-driven split ring.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmix.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "boot.h"
#include "kalloc.h"
#include "printk.h"
#include "intc.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

#define VIRTIO_MAGIC        0x74726976U
#define VIRTIO_MMIO_MODERN  2U
#define VIRTIO_BLOCK_DEVICE 2U
#define VIRTIO_QEMU_VENDOR  0x554d4551U

#define VIRTIO_SECTOR_SIZE         512U
#define VIRTIO_SECTORS_PER_BLOCK   (BSIZE / VIRTIO_SECTOR_SIZE)
#define VIRTIO_XV6_SECTORS         ((uint64)FSSIZE * VIRTIO_SECTORS_PER_BLOCK)
#define VIRTIO_CONFIG_READ_RETRIES 16
#define VIRTIO_DESCRIPTORS_PER_REQ 3
#define VIRTIO_DMA_SYNC_BYTES      256

#define VIRTIO_ISR_QUEUE  1U
#define VIRTIO_ISR_CONFIG 2U
#define VIRTIO_ISR_VALID  (VIRTIO_ISR_QUEUE | VIRTIO_ISR_CONFIG)

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

extern char kernel_rodata_end[];
extern char kernel_end[];

// A dedicated cache-maintenance span prevents device status writes from
// sharing a synchronized region with driver-only ownership state.
struct virtio_request_wire {
  struct virtio_blk_req request;
  uint8 status;
  uint8 padding[VIRTIO_DMA_SYNC_BYTES - sizeof(struct virtio_blk_req) - 1];
} __attribute__((aligned(VIRTIO_DMA_SYNC_BYTES)));

struct virtio_request_info {
  struct buf *b;
  uint16 desc[VIRTIO_DESCRIPTORS_PER_REQ];
  uint8 write;
  uint8 active;
};

static struct {
  struct spinlock lock;

  // The driver publishes chains through desc and avail; used reports results.
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
  void *data[NUM];

  struct virtio_request_wire wire[NUM];
  struct virtio_request_info info[NUM];
  uint8 free[NUM];
  uint16 avail_idx;
  uint16 used_idx;
  uint outstanding;

  uint64 capacity;
  uint32 offered[2];
  uint32 accepted[2];
  uint32 queue_max;
  uint32 status;
  uint32 status_trace[5];
  uint status_trace_count;
  int ready;
} disk;

static uint32
virtio_bswap32(uint32 value)
{
  return (value & 0x000000ffU) << 24 | (value & 0x0000ff00U) << 8 |
         (value & 0x00ff0000U) >> 8 | (value & 0xff000000U) >> 24;
}

static int
virtio_platform_valid(void)
{
  return mmix_boot.bootinfo_status == MMIX_BOOTINFO_OK &&
         mmix_boot.info.virtio_mmio_base == VIRTIO0_BASE &&
         mmix_boot.info.virtio_mmio_irq == VIRTIO0_IRQ &&
         mmix_boot.info.virtio_mmio_count == VIRTIO_MMIO_COUNT;
}

static volatile uint32 *
virtio_register(uint offset)
{
  if (!virtio_platform_valid() || (offset & (sizeof(uint32) - 1)) != 0 ||
      offset > VIRTIO0_SIZE - sizeof(uint32))
    panic("virtio register");
  return (volatile uint32 *)(mmix_boot.info.virtio_mmio_base + offset);
}

// QEMU exposes modern VirtIO MMIO as little-endian 32-bit registers. A native
// MMIX tetra access observes the byte-swapped representation.
static uint32
virtio_read(uint offset)
{
  return virtio_bswap32(*virtio_register(offset));
}

static void
virtio_write(uint offset, uint32 value)
{
  *virtio_register(offset) = virtio_bswap32(value);
}

static void virtio_fail(char *message) __attribute__((noreturn));

static void
virtio_record_status(uint32 status)
{
  if (disk.status_trace_count >=
      sizeof(disk.status_trace) / sizeof(disk.status_trace[0]))
    virtio_fail("virtio status trace");
  disk.status_trace[disk.status_trace_count++] = status;
}

static void
virtio_set_status(uint32 bit)
{
  uint32 actual;

  disk.status |= bit;
  virtio_write(VIRTIO_MMIO_STATUS, disk.status);
  actual = virtio_read(VIRTIO_MMIO_STATUS);
  if ((actual & disk.status) != disk.status ||
      (actual & (VIRTIO_CONFIG_S_DEVICE_NEEDS_RESET |
                 VIRTIO_CONFIG_S_FAILED)) != 0)
    virtio_fail("virtio status");
  disk.status = actual;
  virtio_record_status(actual);
}

static void
virtio_fail(char *message)
{
  uint32 status = virtio_read(VIRTIO_MMIO_STATUS);

  // A responsive transport must observe FAILED before the driver stops.
  virtio_write(VIRTIO_MMIO_STATUS, status | VIRTIO_CONFIG_S_FAILED);
  panic(message);
}

static uint32
virtio_read_features(uint word)
{
  virtio_write(VIRTIO_MMIO_DEVICE_FEATURES_SEL, word);
  return virtio_read(VIRTIO_MMIO_DEVICE_FEATURES);
}

static void
virtio_write_features(uint word, uint32 features)
{
  virtio_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, word);
  virtio_write(VIRTIO_MMIO_DRIVER_FEATURES, features);
}

static uint64
virtio_read_capacity(void)
{
  // Retry if the device changes its configuration during the two-word read.
  for (int retry = 0; retry < VIRTIO_CONFIG_READ_RETRIES; retry++) {
    uint32 generation = virtio_read(VIRTIO_MMIO_CONFIG_GENERATION);
    uint32 low = virtio_read(VIRTIO_MMIO_CONFIG);
    uint32 high = virtio_read(VIRTIO_MMIO_CONFIG + sizeof(uint32));

    if (generation == virtio_read(VIRTIO_MMIO_CONFIG_GENERATION))
      return (uint64)low | (uint64)high << 32;
  }
  virtio_fail("virtio config change");
}

static uint64
virtio_dma_page_address(void *page)
{
  uint64 address = (uint64)page;

  if (!kalloc_page_is_dma(page) || (address & (PGSIZE - 1)) != 0 ||
      (address & MMIX_PHYSICAL_ALIAS_BIT) != 0 || address >= LOW_RAM_END)
    virtio_fail("virtio dma page");
  return address;
}

static uint64
virtio_dma_static_address(void *storage, uint length, uint alignment)
{
  uint64 address = (uint64)storage;
  uint64 limit = address + length;

  if (length == 0 || alignment == 0 ||
      (alignment & (alignment - 1)) != 0 ||
      (address & (alignment - 1)) != 0 || limit < address ||
      address < (uint64)kernel_rodata_end || limit > (uint64)kernel_end ||
      (address & MMIX_PHYSICAL_ALIAS_BIT) != 0 || limit > LOW_RAM_END)
    virtio_fail("virtio dma static");
  return address;
}

// Every synchronized object is isolated in an aligned 256-byte span or an
// allocator page, so cache maintenance never overlaps unrelated ownership.
static void
virtio_dma_sync(uint64 address, uint length, int device_wrote)
{
  if (length == 0 || (address & (VIRTIO_DMA_SYNC_BYTES - 1)) != 0 ||
      (length & (VIRTIO_DMA_SYNC_BYTES - 1)) != 0 ||
      address + length < address || address + length > LOW_RAM_END)
    virtio_fail("virtio dma sync");
  if (device_wrote)
    address |= MMIX_PHYSICAL_ALIAS_BIT;

  for (uint offset = 0; offset < length; offset += VIRTIO_DMA_SYNC_BYTES)
    asm volatile("SYNCD 255,%0,0" : : "r"(address + offset) : "memory");
  mmix_sync_memory();
}

static void
virtio_dma_publish_page(void *page)
{
  virtio_dma_sync(virtio_dma_page_address(page), PGSIZE, 0);
}

static void
virtio_dma_consume_page(void *page)
{
  virtio_dma_sync(virtio_dma_page_address(page), PGSIZE, 1);
}

static void
virtio_dma_publish_wire(uint head)
{
  uint64 address = virtio_dma_static_address(
    &disk.wire[head], sizeof(disk.wire[head]), VIRTIO_DMA_SYNC_BYTES);

  virtio_dma_sync(address, sizeof(disk.wire[head]), 0);
}

static void
virtio_dma_consume_wire(uint head)
{
  uint64 address = virtio_dma_static_address(
    &disk.wire[head], sizeof(disk.wire[head]), VIRTIO_DMA_SYNC_BYTES);

  virtio_dma_sync(address, sizeof(disk.wire[head]), 1);
}

static void
virtio_release_pages(void)
{
  for (uint i = 0; i < NUM; i++) {
    if (disk.data[i] != 0) {
      kfree(disk.data[i]);
      disk.data[i] = 0;
    }
  }
  if (disk.used != 0) {
    kfree(disk.used);
    disk.used = 0;
  }
  if (disk.avail != 0) {
    kfree(disk.avail);
    disk.avail = 0;
  }
  if (disk.desc != 0) {
    kfree(disk.desc);
    disk.desc = 0;
  }
}

static int
virtio_allocate_pages(void)
{
  disk.desc = kalloc_dma();
  disk.avail = kalloc_dma();
  disk.used = kalloc_dma();
  if (disk.desc == 0 || disk.avail == 0 || disk.used == 0) {
    virtio_release_pages();
    return -1;
  }
  for (uint i = 0; i < NUM; i++) {
    disk.data[i] = kalloc_dma();
    if (disk.data[i] == 0) {
      virtio_release_pages();
      return -1;
    }
  }

  memset(disk.desc, 0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);
  return 0;
}

static void
virtio_write_address(uint low_register, uint64 address)
{
  virtio_write(low_register, (uint32)address);
  virtio_write(low_register + sizeof(uint32), (uint32)(address >> 32));
}

static int
virtio_alloc_desc(void)
{
  for (uint i = 0; i < NUM; i++) {
    if (disk.free[i]) {
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

static void
virtio_free_desc(uint i)
{
  if (i >= NUM || disk.free[i])
    virtio_fail("virtio free descriptor");
  memset(&disk.desc[i], 0, sizeof(disk.desc[i]));
  disk.free[i] = 1;
}

static int
virtio_alloc_chain(uint16 chain[VIRTIO_DESCRIPTORS_PER_REQ])
{
  for (uint i = 0; i < VIRTIO_DESCRIPTORS_PER_REQ; i++) {
    int descriptor = virtio_alloc_desc();

    if (descriptor < 0) {
      while (i != 0)
        virtio_free_desc(chain[--i]);
      return -1;
    }
    chain[i] = descriptor;
  }
  return 0;
}

static void
virtio_free_chain(struct virtio_request_info *info)
{
  if (info->desc[0] >= NUM || info->desc[1] >= NUM || info->desc[2] >= NUM ||
      info->desc[0] == info->desc[1] || info->desc[0] == info->desc[2] ||
      info->desc[1] == info->desc[2])
    virtio_fail("virtio free chain");
  for (uint i = 0; i < VIRTIO_DESCRIPTORS_PER_REQ; i++)
    virtio_free_desc(info->desc[i]);
}

static void
virtio_set_desc(uint index, uint64 address, uint32 length, uint16 flags,
                uint16 next)
{
  struct virtq_desc *descriptor;

  if (index >= NUM || disk.free[index] || address + length < address ||
      address + length > LOW_RAM_END || next >= NUM)
    virtio_fail("virtio descriptor");
  descriptor = &disk.desc[index];
  virtio_store_le64(descriptor->addr, address);
  virtio_store_le32(descriptor->len, length);
  virtio_store_le16(descriptor->flags, flags);
  virtio_store_le16(descriptor->next, next);
}

static int
virtio_desc_matches(uint index, uint64 address, uint32 length, uint16 flags,
                    uint16 next)
{
  struct virtq_desc *descriptor;

  if (index >= NUM || next >= NUM)
    return 0;
  descriptor = &disk.desc[index];
  return virtio_load_le64(descriptor->addr) == address &&
         virtio_load_le32(descriptor->len) == length &&
         virtio_load_le16(descriptor->flags) == flags &&
         virtio_load_le16(descriptor->next) == next;
}

static uint64
virtio_request_sector(struct buf *b)
{
  uint64 sector;

  if (b == 0 || b->blockno >= FSSIZE)
    virtio_fail("virtio block range");
  sector = (uint64)b->blockno * VIRTIO_SECTORS_PER_BLOCK;
  if (sector > disk.capacity ||
      VIRTIO_SECTORS_PER_BLOCK > disk.capacity - sector)
    virtio_fail("virtio sector range");
  return sector;
}

static void
virtio_prepare_request(uint head, struct buf *b, int write, uint64 sector)
{
  struct virtio_request_info *info = &disk.info[head];
  struct virtio_request_wire *wire = &disk.wire[head];
  uint16 data_flags = VRING_DESC_F_NEXT;
  uint64 wire_address;
  uint64 data_address;

  if (info->active || info->b != 0 || b->disk)
    virtio_fail("virtio request owner");
  if (!write)
    data_flags |= VRING_DESC_F_WRITE;

  info->b = b;
  info->write = write;
  info->active = 1;
  wire->status = 0xff;
  virtio_store_le32(wire->request.type,
                    write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN);
  virtio_store_le32(wire->request.reserved, 0);
  virtio_store_le64(wire->request.sector, sector);
  if (write)
    memmove(disk.data[head], b->data, BSIZE);

  wire_address = virtio_dma_static_address(
    wire, sizeof(*wire), VIRTIO_DMA_SYNC_BYTES);
  data_address = virtio_dma_page_address(disk.data[head]);
  virtio_set_desc(info->desc[0], wire_address,
                  sizeof(struct virtio_blk_req), VRING_DESC_F_NEXT,
                  info->desc[1]);
  virtio_set_desc(info->desc[1], data_address, BSIZE, data_flags,
                  info->desc[2]);
  virtio_set_desc(info->desc[2], wire_address +
                    __builtin_offsetof(struct virtio_request_wire, status),
                  1, VRING_DESC_F_WRITE, 0);
  b->disk = 1;

  // Flush every cache span before publishing the chain to the available ring.
  virtio_dma_publish_wire(head);
  virtio_dma_publish_page(disk.data[head]);
  virtio_dma_publish_page(disk.desc);
}

static int
virtio_chain_matches(uint head)
{
  struct virtio_request_info *info = &disk.info[head];
  struct virtio_request_wire *wire = &disk.wire[head];
  uint64 wire_address = (uint64)wire;
  uint64 data_address = (uint64)disk.data[head];
  uint16 data_flags = VRING_DESC_F_NEXT;

  if (!info->active || info->b == 0 || info->desc[0] != head ||
      info->desc[1] >= NUM || info->desc[2] >= NUM ||
      head == info->desc[1] || head == info->desc[2] ||
      info->desc[1] == info->desc[2])
    return 0;
  if (!info->write)
    data_flags |= VRING_DESC_F_WRITE;
  return virtio_desc_matches(head, wire_address,
                             sizeof(struct virtio_blk_req),
                             VRING_DESC_F_NEXT, info->desc[1]) &&
         virtio_desc_matches(info->desc[1], data_address, BSIZE, data_flags,
                             info->desc[2]) &&
         virtio_desc_matches(
           info->desc[2],
           wire_address +
             __builtin_offsetof(struct virtio_request_wire, status),
           1, VRING_DESC_F_WRITE, 0);
}

static void
virtio_publish_request(uint head)
{
  uint slot = disk.avail_idx % NUM;

  virtio_store_le16(disk.avail->ring[slot], head);
  virtio_dma_publish_page(disk.avail);
  virtio_store_le16(disk.avail->idx, ++disk.avail_idx);
  virtio_dma_publish_page(disk.avail);
  virtio_write(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

static void
virtio_revalidate_config(void)
{
  uint32 status = virtio_read(VIRTIO_MMIO_STATUS);
  uint64 capacity = virtio_read_capacity();

  if ((status & (VIRTIO_CONFIG_S_DRIVER_OK |
                 VIRTIO_CONFIG_S_DEVICE_NEEDS_RESET |
                 VIRTIO_CONFIG_S_FAILED)) != VIRTIO_CONFIG_S_DRIVER_OK ||
      capacity < VIRTIO_XV6_SECTORS)
    virtio_fail("virtio config status");
  for (uint head = 0; head < NUM; head++) {
    struct virtio_request_info *info = &disk.info[head];

    if (info->active) {
      uint64 sector = virtio_load_le64(disk.wire[head].request.sector);

      if (sector > capacity ||
          VIRTIO_SECTORS_PER_BLOCK > capacity - sector)
        virtio_fail("virtio config range");
    }
  }
  disk.capacity = capacity;
  disk.status = status;
}

static void
virtio_complete_request(uint head, uint32 used_length)
{
  struct virtio_request_info *info;
  struct virtio_request_wire *wire;
  struct buf *b;
  uint32 expected_length;

  if (head >= NUM)
    virtio_fail("virtio used id");
  info = &disk.info[head];
  if (!virtio_chain_matches(head))
    virtio_fail("virtio used chain");
  wire = &disk.wire[head];
  b = info->b;
  expected_length = info->write ? 1 : BSIZE + 1;
  if (used_length != expected_length)
    virtio_fail("virtio used length");

  virtio_dma_consume_wire(head);
  if (!info->write)
    virtio_dma_consume_page(disk.data[head]);
  if (wire->status == VIRTIO_BLK_S_IOERR)
    virtio_fail("virtio io error");
  if (wire->status == VIRTIO_BLK_S_UNSUPP)
    virtio_fail("virtio unsupported");
  if (wire->status != VIRTIO_BLK_S_OK)
    virtio_fail("virtio invalid status");
  if (!info->write)
    memmove(b->data, disk.data[head], BSIZE);

  b->disk = 0;
  virtio_free_chain(info);
  info->b = 0;
  info->write = 0;
  info->active = 0;
  if (disk.outstanding == 0)
    virtio_fail("virtio outstanding");
  disk.outstanding--;
  wakeup(b);
  wakeup(&disk.free[0]);
}

static void
virtio_consume_used(void)
{
  uint16 device_idx;
  uint16 count;

  virtio_dma_consume_page(disk.used);
  device_idx = virtio_load_le16(disk.used->idx);
  count = (uint16)(device_idx - disk.used_idx);
  // A completion arriving after ISR acknowledgement but before this snapshot
  // can be consumed here while leaving a redundant queue notification.
  if (count == 0)
    return;
  if (count > NUM || count > disk.outstanding)
    virtio_fail("virtio used index");

  while (disk.used_idx != device_idx) {
    struct virtq_used_elem *used = &disk.used->ring[disk.used_idx % NUM];
    uint32 head;
    uint32 length;

    virtio_dma_consume_page(disk.used);
    head = virtio_load_le32(used->id);
    length = virtio_load_le32(used->len);
    virtio_complete_request(head, length);
    disk.used_idx++;
  }
}

void
virtio_disk_init(void)
{
  uint32 magic;
  uint32 version;
  uint32 device;
  uint32 vendor;
  uint64 desc_address;
  uint64 avail_address;
  uint64 used_address;

  if (disk.ready)
    panic("virtio duplicate init");
  initlock(&disk.lock, "virtio_disk");

  magic = virtio_read(VIRTIO_MMIO_MAGIC_VALUE);
  version = virtio_read(VIRTIO_MMIO_VERSION);
  device = virtio_read(VIRTIO_MMIO_DEVICE_ID);
  vendor = virtio_read(VIRTIO_MMIO_VENDOR_ID);
  if (magic != VIRTIO_MAGIC)
    panic("virtio magic");
  if (version != VIRTIO_MMIO_MODERN)
    panic("virtio version");
  if (device != VIRTIO_BLOCK_DEVICE)
    virtio_fail("virtio device");
  if (vendor != VIRTIO_QEMU_VENDOR)
    virtio_fail("virtio vendor");

  disk.status = 0;
  virtio_write(VIRTIO_MMIO_STATUS, 0);
  if (virtio_read(VIRTIO_MMIO_STATUS) != 0)
    virtio_fail("virtio reset");
  virtio_record_status(0);
  virtio_set_status(VIRTIO_CONFIG_S_ACKNOWLEDGE);
  virtio_set_status(VIRTIO_CONFIG_S_DRIVER);

  // Negotiate both feature words before accepting the modern-only contract.
  disk.offered[0] = virtio_read_features(0);
  disk.offered[1] = virtio_read_features(1);
  if ((disk.offered[VIRTIO_FEATURE_WORD(VIRTIO_BLK_F_RO)] &
       VIRTIO_FEATURE_MASK(VIRTIO_BLK_F_RO)) != 0)
    virtio_fail("virtio read only");
  if ((disk.offered[VIRTIO_FEATURE_WORD(VIRTIO_F_VERSION_1)] &
       VIRTIO_FEATURE_MASK(VIRTIO_F_VERSION_1)) == 0)
    virtio_fail("virtio version feature");

  disk.accepted[0] = 0;
  disk.accepted[1] = VIRTIO_FEATURE_MASK(VIRTIO_F_VERSION_1);
  virtio_write_features(0, disk.accepted[0]);
  virtio_write_features(1, disk.accepted[1]);
  disk.status |= VIRTIO_CONFIG_S_FEATURES_OK;
  virtio_write(VIRTIO_MMIO_STATUS, disk.status);
  disk.status = virtio_read(VIRTIO_MMIO_STATUS);
  if ((disk.status & VIRTIO_CONFIG_S_FEATURES_OK) == 0)
    virtio_fail("virtio features rejected");
  if ((disk.status & (VIRTIO_CONFIG_S_DEVICE_NEEDS_RESET |
                      VIRTIO_CONFIG_S_FAILED)) != 0)
    virtio_fail("virtio feature status");
  virtio_record_status(disk.status);

  disk.capacity = virtio_read_capacity();
  if (disk.capacity < VIRTIO_XV6_SECTORS)
    virtio_fail("virtio capacity");

  // Configure queue zero completely before making it ready to the device.
  virtio_write(VIRTIO_MMIO_QUEUE_SEL, 0);
  if (virtio_read(VIRTIO_MMIO_QUEUE_READY) != 0)
    virtio_fail("virtio queue busy");
  disk.queue_max = virtio_read(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if (disk.queue_max < NUM)
    virtio_fail("virtio queue size");
  if (virtio_allocate_pages() < 0)
    virtio_fail("virtio queue alloc");

  for (uint i = 0; i < NUM; i++) {
    disk.free[i] = 1;
    virtio_dma_page_address(disk.data[i]);
    virtio_dma_static_address(&disk.wire[i], sizeof(disk.wire[i]),
                              VIRTIO_DMA_SYNC_BYTES);
  }
  desc_address = virtio_dma_page_address(disk.desc);
  avail_address = virtio_dma_page_address(disk.avail);
  used_address = virtio_dma_page_address(disk.used);
  virtio_dma_publish_page(disk.desc);
  virtio_dma_publish_page(disk.avail);
  virtio_dma_publish_page(disk.used);

  virtio_write(VIRTIO_MMIO_QUEUE_NUM, NUM);
  virtio_write_address(VIRTIO_MMIO_QUEUE_DESC_LOW, desc_address);
  virtio_write_address(VIRTIO_MMIO_QUEUE_DRIVER_LOW, avail_address);
  virtio_write_address(VIRTIO_MMIO_QUEUE_DEVICE_LOW, used_address);
  mmix_sync_memory();
  virtio_write(VIRTIO_MMIO_QUEUE_READY, 1);
  if (virtio_read(VIRTIO_MMIO_QUEUE_READY) != 1)
    virtio_fail("virtio queue publish");
  if (intc_set_enabled(VIRTIO0_IRQ, 1) != MMIX_INTC_OK)
    virtio_fail("virtio irq enable");

  // DRIVER_OK is the final transition after queue and interrupt publication.
  virtio_set_status(VIRTIO_CONFIG_S_DRIVER_OK);
  disk.ready = 1;

  printk("virtio-mmio: magic=%x version=%u device=%u vendor=%x ", magic,
         version, device, vendor);
  printk("features=%x:%x accepted=%x:%x status=%x>%x>%x>%x>%x\n",
         disk.offered[1], disk.offered[0], disk.accepted[1], disk.accepted[0],
         disk.status_trace[0], disk.status_trace[1], disk.status_trace[2],
         disk.status_trace[3], disk.status_trace[4]);
  printk("virtio-blk: sectors=%lu queue=%u/%u desc=%p avail=%p used=%p\n",
         disk.capacity, NUM, disk.queue_max, (void *)desc_address,
         (void *)avail_address, (void *)used_address);
}

void
virtio_disk_rw(struct buf *b, int write)
{
  uint16 chain[VIRTIO_DESCRIPTORS_PER_REQ];
  uint64 sector;
  uint head;

  if (!disk.ready || b == 0 || (write != 0 && write != 1) || myproc() == 0)
    virtio_fail("virtio request");
  sector = virtio_request_sector(b);

  acquire(&disk.lock);
  while (virtio_alloc_chain(chain) < 0)
    sleep(&disk.free[0], &disk.lock);
  head = chain[0];
  for (uint i = 0; i < VIRTIO_DESCRIPTORS_PER_REQ; i++)
    disk.info[head].desc[i] = chain[i];
  virtio_prepare_request(head, b, write, sector);
  if (disk.outstanding >= NUM / VIRTIO_DESCRIPTORS_PER_REQ)
    virtio_fail("virtio request count");
  disk.outstanding++;
  virtio_publish_request(head);

  while (b->disk)
    sleep(b, &disk.lock);
  release(&disk.lock);
}

void
virtio_disk_intr(void)
{
  uint32 interrupt;

  acquire(&disk.lock);
  if (!disk.ready)
    virtio_fail("virtio interrupt state");
  interrupt = virtio_read(VIRTIO_MMIO_INTERRUPT_STATUS);
  if (interrupt == 0)
    virtio_fail("virtio empty interrupt");
  // Acknowledge exactly the bits observed before consuming device output.
  virtio_write(VIRTIO_MMIO_INTERRUPT_ACK, interrupt);
  if ((interrupt & ~VIRTIO_ISR_VALID) != 0)
    virtio_fail("virtio interrupt bits");
  if ((interrupt & VIRTIO_ISR_CONFIG) != 0)
    virtio_revalidate_config();
  if ((interrupt & VIRTIO_ISR_QUEUE) != 0)
    virtio_consume_used();
  release(&disk.lock);
}

_Static_assert(BSIZE == 1024 && VIRTIO_SECTOR_SIZE == 512 &&
                 VIRTIO_SECTORS_PER_BLOCK == 2,
               "VirtIO and xv6 block geometry mismatch");
_Static_assert(sizeof(struct virtio_request_wire) == VIRTIO_DMA_SYNC_BYTES &&
                 __alignof__(struct virtio_request_wire) ==
                   VIRTIO_DMA_SYNC_BYTES &&
                 __builtin_offsetof(struct virtio_request_wire, request) == 0 &&
                 __builtin_offsetof(struct virtio_request_wire, status) == 16,
               "VirtIO request DMA isolation mismatch");
_Static_assert(NUM >= VIRTIO_DESCRIPTORS_PER_REQ &&
                 NUM / VIRTIO_DESCRIPTORS_PER_REQ >= 2,
               "VirtIO queue cannot hold concurrent requests");
