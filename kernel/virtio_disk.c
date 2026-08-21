// Modern VirtIO MMIO block-device negotiation and split-ring setup.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmix.h"
#include "spinlock.h"
#include "boot.h"
#include "kalloc.h"
#include "printk.h"
#include "defs.h"
#include "virtio.h"

#define VIRTIO_MAGIC       0x74726976U
#define VIRTIO_MMIO_MODERN 2U
#define VIRTIO_BLOCK_DEVICE 2U
#define VIRTIO_QEMU_VENDOR 0x554d4551U
#define VIRTIO_XV6_SECTORS 4000ULL
#define VIRTIO_CONFIG_READ_RETRIES 16

static struct {
  struct spinlock lock;
  // The driver publishes chains through desc and avail; used reports results.
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
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

  // A responsive transport must observe FAILED before initialization stops.
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
virtio_dma_address(void *page)
{
  uint64 address = (uint64)page;

  if (!kalloc_page_is_managed(page) || (address & (PGSIZE - 1)) != 0 ||
      (address & MMIX_PHYSICAL_ALIAS_BIT) != 0 || address >= LOW_RAM_END)
    virtio_fail("virtio dma address");
  return address;
}

// Publish one allocator page for a device that may read it. The fixed 256-byte
// spans match SYNCD's maximum immediate range and divide an MMIX page exactly.
static void
virtio_publish_page(void *page)
{
  uint64 address = (uint64)page;

  for (uint offset = 0; offset < PGSIZE; offset += 256)
    asm volatile("SYNCD 255,%0,0" : : "r"(address + offset) : "memory");
  mmix_sync_memory();
}

static int
virtio_allocate_queue(void)
{
  disk.desc = kalloc();
  disk.avail = kalloc();
  disk.used = kalloc();
  if (disk.desc == 0 || disk.avail == 0 || disk.used == 0) {
    if (disk.used != 0)
      kfree(disk.used);
    if (disk.avail != 0)
      kfree(disk.avail);
    if (disk.desc != 0)
      kfree(disk.desc);
    disk.desc = 0;
    disk.avail = 0;
    disk.used = 0;
    return -1;
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
  if (virtio_allocate_queue() < 0)
    virtio_fail("virtio queue alloc");

  desc_address = virtio_dma_address(disk.desc);
  avail_address = virtio_dma_address(disk.avail);
  used_address = virtio_dma_address(disk.used);
  virtio_publish_page(disk.desc);
  virtio_publish_page(disk.avail);
  virtio_publish_page(disk.used);

  virtio_write(VIRTIO_MMIO_QUEUE_NUM, NUM);
  virtio_write_address(VIRTIO_MMIO_QUEUE_DESC_LOW, desc_address);
  virtio_write_address(VIRTIO_MMIO_QUEUE_DRIVER_LOW, avail_address);
  virtio_write_address(VIRTIO_MMIO_QUEUE_DEVICE_LOW, used_address);
  mmix_sync_memory();
  virtio_write(VIRTIO_MMIO_QUEUE_READY, 1);
  if (virtio_read(VIRTIO_MMIO_QUEUE_READY) != 1)
    virtio_fail("virtio queue publish");

  // DRIVER_OK is the final transition after the queue is fully published.
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
