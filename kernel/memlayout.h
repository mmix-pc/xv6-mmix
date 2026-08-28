#ifndef XV6_MMIX_MEMLAYOUT_H
#define XV6_MMIX_MEMLAYOUT_H

// QEMU MMIX virt physical memory map. Intervals are half-open and the drawing
// is not to scale.
//
//   0x0000000000000000 +----------------------------------+
//                      | Low RAM (96 MiB)                 |
//   0x0000000006000000 +----------------------------------+
//                      | Pool Segment backing (8 MiB)     |
//   0x0000000006800000 +----------------------------------+
//                      | Data Segment backing (64 MiB)    |
//   0x000000000a800000 +----------------------------------+
//                      | Stack Segment backing (64 MiB)   |
//   0x000000000e800000 +----------------------------------+
//                      | Platform RAM (8 MiB; boot info)  |
//   0x000000000f000000 +----------------------------------+
//                      | Framebuffer backing (16 MiB)     |
//   0x0000000010000000 +----------------------------------+ minimum RAM end
//                      | UART MMIO and reserved gaps      |
//   0x0000000010001000 +----------------------------------+
//                      | VirtIO MMIO (4 KiB)              |
//   0x0000000010002000 +----------------------------------+
//                      | Framebuffer control (4 KiB)      |
//   0x0000000010003000 +----------------------------------+
//                      | Timer MMIO (4 KiB)               |
//   0x0000000010004000 +----------------------------------+
//                      | INTC MMIO (8 KiB)                |
//   0x0000000010006000 +----------------------------------+
//                      | IPI MMIO (4 KiB)                 |
//   0x0000000010007000 +----------------------------------+
//                      | Reserved MMIO page padding       |
//   0x0000000010008000 +----------------------------------+
//                      | Reserved MMIO aperture           |
//   0x0000000020000000 +----------------------------------+
//                      | Optional High RAM                |
//     larger RAM's end +----------------------------------+
#define LOW_RAM_BASE 0x0000000000000000
#define LOW_RAM_SIZE 0x0000000006000000
#define LOW_RAM_END (LOW_RAM_BASE + LOW_RAM_SIZE)

#define POOL_LOGICAL_BASE 0x4000000000000000
#define POOL_PHYS_BASE 0x0000000006000000
#define POOL_SIZE 0x0000000000800000
#define POOL_PHYS_END (POOL_PHYS_BASE + POOL_SIZE)

#define DATA_LOGICAL_BASE 0x2000000000000000
#define DATA_PHYS_BASE 0x0000000006800000
#define DATA_SIZE 0x0000000004000000
#define DATA_PHYS_END (DATA_PHYS_BASE + DATA_SIZE)

#define STACK_LOGICAL_BASE 0x6000000000000000
#define STACK_PHYS_BASE 0x000000000a800000
#define STACK_SIZE 0x0000000004000000
#define STACK_PHYS_END (STACK_PHYS_BASE + STACK_SIZE)

// Physical backing for the three bare segments is one adjacent platform
// interval. Paging maps it as ordinary read/write, non-executable memory;
// allocator ownership is established separately after rV becomes live.
#define BARE_SEGMENT_BACKING_BASE POOL_PHYS_BASE
#define BARE_SEGMENT_BACKING_LIMIT STACK_PHYS_END
#define BARE_SEGMENT_BACKING_SIZE                                      \
  (BARE_SEGMENT_BACKING_LIMIT - BARE_SEGMENT_BACKING_BASE)

#define PLATFORM_RAM_BASE 0x000000000e800000
#define PLATFORM_RAM_SIZE 0x0000000000800000
#define PLATFORM_RAM_END (PLATFORM_RAM_BASE + PLATFORM_RAM_SIZE)

#define BOOTINFO_BASE 0x000000000e800000
#define BOOTINFO_SIZE 0x0000000000000168
#define BOOTINFO_END (BOOTINFO_BASE + BOOTINFO_SIZE)

#define FRAMEBUFFER_BASE 0x000000000f000000
#define FRAMEBUFFER_SIZE 0x0000000001000000
#define FRAMEBUFFER_END (FRAMEBUFFER_BASE + FRAMEBUFFER_SIZE)

// The production topology has one boot CPU and up to sixteen configured CPUs.
#define MMIX_MAX_CPUS 16
#define BOOT_CPU_ID 0

// QEMU MMIX virt MMIO map. Device register offsets belong to each driver.
#define MMIO_BASE 0x0000000010000000

#define UART0_BASE 0x0000000010000000
#define UART0_SIZE 0x0000000000000100
#define UART0_IRQ 1

#define VIRTIO0_BASE 0x0000000010001000
#define VIRTIO0_SIZE 0x0000000000001000
#define VIRTIO0_IRQ 2
#define VIRTIO_MMIO_COUNT 1

#define FRAMEBUFFER_CONTROL_BASE 0x0000000010002000
#define FRAMEBUFFER_CONTROL_SIZE 0x0000000000001000
// Reserved by the machine ABI; QEMU does not currently connect this source.
#define FRAMEBUFFER_IRQ 3

#define TIMER_BASE 0x0000000010003000
#define TIMER_SIZE 0x0000000000001000
#define TIMER_IRQ_BASE 16
#define TIMER_IRQ_COUNT_MAX MMIX_MAX_CPUS

#define INTC_BASE 0x0000000010004000
#define INTC_SIZE 0x0000000000002000
#define INTC_IRQ_COUNT 32
#define INTC_SHARED_IRQ_FIRST 1
#define INTC_SHARED_IRQ_LAST 15
#define INTC_CONTEXT_COUNT 16

#define IPI_BASE 0x0000000010006000
#define IPI_SIZE 0x0000000000001000
#define IPI_TARGET_COUNT_MAX MMIX_MAX_CPUS
#define IPI_REQUEST_MASK 0x0000000000000200

// RAM capacity is split around one exclusive 256-MiB MMIO aperture.
#define RAM_MINIMUM_SIZE 0x0000000010000000
#define PHYSICAL_LOW_RAM_BASE 0x0000000000000000
#define PHYSICAL_LOW_RAM_SIZE RAM_MINIMUM_SIZE
#define PHYSICAL_LOW_RAM_END (PHYSICAL_LOW_RAM_BASE + PHYSICAL_LOW_RAM_SIZE)
#define MMIO_APERTURE_BASE 0x0000000010000000
#define MMIO_APERTURE_SIZE 0x0000000010000000
#define MMIO_APERTURE_END (MMIO_APERTURE_BASE + MMIO_APERTURE_SIZE)
#define PHYSICAL_HIGH_RAM_BASE MMIO_APERTURE_END
// The current kernel table construction uses the level-1 root directly.
#define KERNEL_IDENTITY_LIMIT 0x0000000200000000

// Bootstrap physical layout within Low RAM. The kernel image ends before the
// allocator begins; that boundary is supplied by the linker.
//
//   0x0000000000000000 +----------------------------------+
//                      | Reserved low-vector page (8 KiB) |
//   0x0000000000002000 +----------------------------------+
//                      | Kernel root tables (24 KiB)      |
//   0x0000000000008000 +----------------------------------+
//                      | Reserved gap (32 KiB)            |
//   0x0000000000010000 +----------------------------------+
//                      | Bootstrap register-stack slots   |
//   0x0000000000100000 +----------------------------------+ KERNEL_LOAD
//                      | Kernel image                     |
//                      +----------------------------------+ KALLOC_START(end)
//                      | Free pages below boot stacks     |
//   0x0000000005fe0000 +----------------------------------+ KERNEL_LIMIT
//                      | 16 bootstrap stacks (128 KiB)    |
//   0x0000000006000000 +----------------------------------+
#define MMIX_PAGE_SIZE 0x0000000000002000
#define MMIX_PAGE_SHIFT 13

// The alignment must be a power of two.
#define ROUNDUP(value, alignment)                                             \
  (((value) + (alignment) - 1) & ~((alignment) - 1))
#define ROUNDDOWN(value, alignment) ((value) & ~((alignment) - 1))

// MMIX pages containing declared device registers are mapped in full. The
// rest of the MMIO aperture remains unmapped.
#define MMIO_DEVICE_PAGE_END ROUNDUP(IPI_BASE + IPI_SIZE, MMIX_PAGE_SIZE)

// Reserve the first physical page for MMIX's fixed low TRIP vectors. The
// kernel does not map the corresponding virtual page until TRIP support exists.
#define MMIX_LOW_VECTOR_BASE LOW_RAM_BASE
#define MMIX_LOW_VECTOR_SIZE MMIX_PAGE_SIZE
#define MMIX_LOW_VECTOR_LIMIT (MMIX_LOW_VECTOR_BASE + MMIX_LOW_VECTOR_SIZE)

// The kernel rV layout uses three contiguous physical root-table blocks in
// the existing low-address guard. Indirect child tables come from kalloc.
#define KERNEL_ROOT_BASE MMIX_LOW_VECTOR_LIMIT
#define KERNEL_ROOT_BLOCKS 3
#define KERNEL_ROOT_SIZE (KERNEL_ROOT_BLOCKS * MMIX_PAGE_SIZE)
#define KERNEL_ROOT_LIMIT (KERNEL_ROOT_BASE + KERNEL_ROOT_SIZE)

#define REGISTER_STACK_BASE 0x0000000000010000
#define REGISTER_STACK_LIMIT 0x0000000000100000
#define BOOT_REGISTER_STACK_STRIDE 0x0000000000008000
#define BOOT_REGISTER_STACK_SIZE BOOT_REGISTER_STACK_STRIDE
#define BOOT_REGISTER_STACK_BASE(cpu_id)                                    \
  (REGISTER_STACK_BASE + (cpu_id) * BOOT_REGISTER_STACK_STRIDE)
#define BOOT_REGISTER_STACK_LIMIT(cpu_id)                                   \
  (BOOT_REGISTER_STACK_BASE(cpu_id) + BOOT_REGISTER_STACK_SIZE)

#define KERNEL_LOAD 0x0000000000100000
#define KERNEL_ENTRY KERNEL_LOAD
#define KERNEL_LIMIT 0x0000000005fe0000

#define BOOT_STACK_SIZE MMIX_PAGE_SIZE
#define BOOT_STACK_COUNT MMIX_MAX_CPUS
#define BOOT_STACK_AREA_SIZE (BOOT_STACK_COUNT * BOOT_STACK_SIZE)
#define BOOT_STACK_AREA_BASE (LOW_RAM_END - BOOT_STACK_AREA_SIZE)
#define BOOT_STACK_AREA_TOP LOW_RAM_END
#define BOOT_STACK_BASE(cpu_id)                                             \
  (BOOT_STACK_AREA_TOP - ((cpu_id) + 1) * BOOT_STACK_SIZE)
#define BOOT_STACK_TOP(cpu_id)                                              \
  (BOOT_STACK_AREA_TOP - (cpu_id) * BOOT_STACK_SIZE)

// Kernel virtual address map. Identity ranges map a virtual address to the
// same physical address. Negative addresses produced by mmix_phys_alias()
// provide privileged direct aliases of physical memory.
//
//   0x0000000000000000 +----------------------------------+
//                      | Unmapped low vectors/tables/gap  |
//   0x0000000000010000 +----------------------------------+
//                      | Identity Low RAM                 |
//   0x0000000006000000 +----------------------------------+
//                      | Identity bare-segment backing    |
//   0x000000000e800000 +----------------------------------+
//                      | Unmapped platform/framebuffer    |
//   0x0000000010000000 +----------------------------------+
//                      | Identity-mapped device pages     |
//   0x0000000010008000 +----------------------------------+
//                      | Unmapped MMIO aperture           |
//   0x0000000020000000 +----------------------------------+
//                      | Optional identity High RAM       |
//     larger RAM's end +----------------------------------+
//                      | Unmapped                         |
//   0x000007ffffd76000 +----------------------------------+
//                      | 65 kernel context slots          |
//   0x0000080000000000 +----------------------------------+
//                      | Unmapped                         |
//   0x8000000000000000 +----------------------------------+
//                      | Direct aliases of mapped RAM     |
//
// Slots 0 through 15 belong to the per-CPU schedulers; slots 16 through 79
// correspond to proc[0..63]. Each slot has two mapped pages separated and
// bounded by guards.
#define MMIX_CONTEXT_AREA_TOP          0x0000080000000000
#define MMIX_CONTEXT_PROCESS_COUNT     64
#define MMIX_CONTEXT_SLOT_COUNT        \
  (MMIX_MAX_CPUS + MMIX_CONTEXT_PROCESS_COUNT)
#define MMIX_CONTEXT_SCHEDULER_SLOT(cpu_id) (cpu_id)
#define MMIX_CONTEXT_PROCESS_SLOT_BASE MMIX_MAX_CPUS
#define MMIX_CONTEXT_SLOT_PAGES        5
#define MMIX_CONTEXT_SLOT_STRIDE (MMIX_CONTEXT_SLOT_PAGES * MMIX_PAGE_SIZE)
#define MMIX_CONTEXT_AREA_SIZE                                                 \
  (MMIX_CONTEXT_SLOT_COUNT * MMIX_CONTEXT_SLOT_STRIDE)
#define MMIX_CONTEXT_AREA_BASE (MMIX_CONTEXT_AREA_TOP - MMIX_CONTEXT_AREA_SIZE)

#define MMIX_CONTEXT_SLOT_TOP(slot)                                            \
  (MMIX_CONTEXT_AREA_TOP - (slot) * MMIX_CONTEXT_SLOT_STRIDE)
#define MMIX_CONTEXT_LOW_GUARD(slot)                                           \
  (MMIX_CONTEXT_SLOT_TOP(slot) - 5 * MMIX_PAGE_SIZE)
#define MMIX_CONTEXT_SOFTWARE_STACK_BASE(slot)                                 \
  (MMIX_CONTEXT_SLOT_TOP(slot) - 4 * MMIX_PAGE_SIZE)
#define MMIX_CONTEXT_SOFTWARE_STACK_TOP(slot)                                  \
  (MMIX_CONTEXT_SLOT_TOP(slot) - 3 * MMIX_PAGE_SIZE)
#define MMIX_CONTEXT_MIDDLE_GUARD(slot) MMIX_CONTEXT_SOFTWARE_STACK_TOP(slot)
#define MMIX_CONTEXT_REGISTER_STACK_BASE(slot)                                 \
  (MMIX_CONTEXT_SLOT_TOP(slot) - 2 * MMIX_PAGE_SIZE)
#define MMIX_CONTEXT_REGISTER_STACK_LIMIT(slot)                                \
  (MMIX_CONTEXT_SLOT_TOP(slot) - MMIX_PAGE_SIZE)
#define MMIX_CONTEXT_HIGH_GUARD(slot) MMIX_CONTEXT_REGISTER_STACK_LIMIT(slot)

#define MMIX_PROCESS_CONTEXT_SLOT(index)                                       \
  (MMIX_CONTEXT_PROCESS_SLOT_BASE + (index))
#define KSTACK(index)                                                          \
  MMIX_CONTEXT_SOFTWARE_STACK_BASE(MMIX_PROCESS_CONTEXT_SLOT(index))
#define MMIX_PROCESS_REGISTER_STACK_BASE(index)                                \
  MMIX_CONTEXT_REGISTER_STACK_BASE(MMIX_PROCESS_CONTEXT_SLOT(index))
#define MMIX_PROCESS_REGISTER_STACK_LIMIT(index)                               \
  MMIX_CONTEXT_REGISTER_STACK_LIMIT(MMIX_PROCESS_CONTEXT_SLOT(index))

// Per-process user virtual address map. Image and heap pages are mapped on
// demand; the empty address ranges shown here consume no physical memory. All
// user addresses outside the ranges below remain unmapped.
//
// Segment 0:
//   0x0000000000000000 +----------------------------------+
//                      | Low guard page (unmapped)        |
//   0x0000000000002000 +----------------------------------+
//                      | Sparse image and heap            |
//   0x00000001ffffc000 +----------------------------------+ heap limit
//                      | Guard page (8 KiB, unmapped)     |
//   0x00000001ffffe000 +----------------------------------+
//                      | Software stack (8 KiB)           |
//   0x0000000200000000 +----------------------------------+
//
// Segment 3 register-stack window:
//   0x600000000000e000 +----------------------------------+
//                      | Low guard page (unmapped)        |
//   0x6000000000010000 +----------------------------------+
//                      | Register stack (128 KiB)         |
//   0x6000000000030000 +----------------------------------+
//                      | High guard page (unmapped)       |
//   0x6000000000032000 +----------------------------------+
#define MMIX_USER_LOW_GUARD_BASE      0x0000000000000000
#define MMIX_USER_IMAGE_BASE          0x0000000000002000
#define MMIX_USER_HEAP_LIMIT          0x00000001ffffc000
#define MMIX_USER_STACK_GUARD_BASE    0x00000001ffffc000
#define MMIX_USER_STACK_BASE          0x00000001ffffe000
#define MMIX_USER_STACK_TOP           0x0000000200000000
#define MMIX_USER_SEGMENT0_LIMIT      MMIX_USER_STACK_TOP
// Keep exec argument data bounded independently of runtime stack capacity.
#define MMIX_EXEC_ARG_MAX             MMIX_PAGE_SIZE
#define MMIX_USER_REGISTER_GUARD_BASE 0x600000000000e000
#define MMIX_USER_REGISTER_STACK_BASE 0x6000000000010000
#define MMIX_USER_REGISTER_STACK_TOP  0x6000000000030000
#define MMIX_USER_REGISTER_GUARD_TOP  0x6000000000032000

#define MMIX_USER_STACK_PAGES                                             \
  ((MMIX_USER_STACK_TOP - MMIX_USER_STACK_BASE) / MMIX_PAGE_SIZE)
#define MMIX_USER_REGISTER_STACK_PAGES                                    \
  ((MMIX_USER_REGISTER_STACK_TOP - MMIX_USER_REGISTER_STACK_BASE) /       \
   MMIX_PAGE_SIZE)

#define KALLOC_START(kernel_end) ROUNDUP(kernel_end, MMIX_PAGE_SIZE)
#define KALLOC_LOW_LIMIT BOOT_STACK_AREA_BASE
#define KALLOC_RECLAIMED_START BARE_SEGMENT_BACKING_BASE
#define KALLOC_RECLAIMED_LIMIT BARE_SEGMENT_BACKING_LIMIT
#define KALLOC_RECLAIMED_PAGES                                      \
  ((KALLOC_RECLAIMED_LIMIT - KALLOC_RECLAIMED_START) / MMIX_PAGE_SIZE)

#if !defined(__ASSEMBLER__)
_Static_assert((MMIX_PAGE_SIZE & (MMIX_PAGE_SIZE - 1)) == 0,
               "MMIX page size must be a power of two");
_Static_assert((KERNEL_ROOT_BASE & (MMIX_PAGE_SIZE - 1)) == 0,
               "kernel root tables must be page-aligned");
_Static_assert(MMIX_LOW_VECTOR_BASE == LOW_RAM_BASE &&
                   MMIX_LOW_VECTOR_SIZE == MMIX_PAGE_SIZE,
               "MMIX low vectors must reserve the first physical page");
_Static_assert(KERNEL_ROOT_BASE == MMIX_LOW_VECTOR_LIMIT,
               "kernel root tables must follow the low-vector page");
_Static_assert(KERNEL_ROOT_SIZE == 0x6000,
               "kernel rV must reserve three root-table blocks");
_Static_assert(KERNEL_ROOT_LIMIT <= REGISTER_STACK_BASE,
               "kernel root tables must remain below the register stack");

_Static_assert(LOW_RAM_END == POOL_PHYS_BASE,
               "Low RAM must end at Pool Segment backing");
_Static_assert(POOL_PHYS_END == DATA_PHYS_BASE,
               "Pool and Data Segment backing must be adjacent");
_Static_assert(DATA_PHYS_END == STACK_PHYS_BASE,
               "Data and Stack Segment backing must be adjacent");
_Static_assert(STACK_PHYS_END == PLATFORM_RAM_BASE,
               "Stack Segment backing must end at platform RAM");
_Static_assert(BARE_SEGMENT_BACKING_BASE == LOW_RAM_END &&
                   BARE_SEGMENT_BACKING_LIMIT == PLATFORM_RAM_BASE &&
                   BARE_SEGMENT_BACKING_SIZE == 0x0000000008800000,
               "bare-segment backing interval must remain contiguous");
_Static_assert((BARE_SEGMENT_BACKING_BASE & (MMIX_PAGE_SIZE - 1)) == 0 &&
                   (BARE_SEGMENT_BACKING_LIMIT &
                    (MMIX_PAGE_SIZE - 1)) == 0,
               "bare-segment backing must use whole MMIX pages");
_Static_assert(PLATFORM_RAM_END == FRAMEBUFFER_BASE,
               "platform RAM must end at the framebuffer");
_Static_assert(FRAMEBUFFER_END == MMIO_BASE,
               "framebuffer memory must end at MMIO");
_Static_assert(BOOTINFO_BASE >= PLATFORM_RAM_BASE &&
                   BOOTINFO_END <= PLATFORM_RAM_END,
               "boot info must fit in platform RAM");

_Static_assert(UART0_BASE >= MMIO_BASE &&
                   UART0_BASE + UART0_SIZE <= VIRTIO0_BASE,
               "UART MMIO range must not overlap VirtIO");
_Static_assert(VIRTIO0_BASE + VIRTIO0_SIZE <= FRAMEBUFFER_CONTROL_BASE,
               "VirtIO MMIO range must not overlap framebuffer control");
_Static_assert(FRAMEBUFFER_CONTROL_BASE + FRAMEBUFFER_CONTROL_SIZE <=
                   TIMER_BASE,
               "framebuffer control must not overlap the timer");
_Static_assert(TIMER_BASE + TIMER_SIZE <= INTC_BASE,
               "timer MMIO range must not overlap the interrupt controller");
_Static_assert(INTC_BASE + INTC_SIZE == IPI_BASE,
               "IPI MMIO must follow the interrupt controller");
_Static_assert(TIMER_IRQ_COUNT_MAX == MMIX_MAX_CPUS &&
                   IPI_TARGET_COUNT_MAX == MMIX_MAX_CPUS &&
                   INTC_CONTEXT_COUNT == MMIX_MAX_CPUS,
               "CPU-local devices must cover the maximum topology");
_Static_assert(RAM_MINIMUM_SIZE == FRAMEBUFFER_END &&
                   (RAM_MINIMUM_SIZE & (MMIX_PAGE_SIZE - 1)) == 0,
               "minimum RAM must contain the fixed physical platform");
_Static_assert(PHYSICAL_LOW_RAM_BASE == 0 &&
                   PHYSICAL_LOW_RAM_END == MMIO_APERTURE_BASE &&
                   MMIO_APERTURE_END == PHYSICAL_HIGH_RAM_BASE &&
                   (PHYSICAL_HIGH_RAM_BASE & (MMIX_PAGE_SIZE - 1)) == 0,
               "physical RAM intervals must surround the MMIO aperture");
_Static_assert(IPI_BASE + IPI_SIZE <= MMIO_DEVICE_PAGE_END &&
                   MMIO_DEVICE_PAGE_END <= MMIO_APERTURE_END &&
                   (MMIO_DEVICE_PAGE_END & (MMIX_PAGE_SIZE - 1)) == 0,
               "device mappings must fit the MMIO aperture");

_Static_assert((KERNEL_LOAD & (MMIX_PAGE_SIZE - 1)) == 0,
               "kernel load address must be page-aligned");
_Static_assert((REGISTER_STACK_BASE & 7) == 0 &&
                   REGISTER_STACK_BASE < REGISTER_STACK_LIMIT,
               "register-stack backing range must be valid and octa-aligned");
_Static_assert(KERNEL_LOAD == REGISTER_STACK_LIMIT,
               "kernel must follow the reserved register-stack range");
_Static_assert(KERNEL_LIMIT == KALLOC_LOW_LIMIT &&
                   KALLOC_LOW_LIMIT == BOOT_STACK_AREA_BASE,
               "kernel limit must stop at the bootstrap stacks");
_Static_assert(MMIX_PAGE_SIZE == (1 << MMIX_PAGE_SHIFT),
               "MMIX page size and shift must agree");
_Static_assert(MMIX_MAX_CPUS <= 256,
               "CPU identities must fit in the rU usage-pattern byte");
_Static_assert(BOOT_STACK_SIZE == MMIX_PAGE_SIZE &&
                   BOOT_STACK_COUNT == MMIX_MAX_CPUS &&
                   BOOT_STACK_AREA_SIZE == 0x20000,
               "the kernel must reserve one bootstrap page per CPU");
_Static_assert(BOOT_STACK_AREA_TOP == LOW_RAM_END &&
                   BOOT_STACK_AREA_BASE == KERNEL_LIMIT &&
                   BOOT_STACK_BASE(0) == 0x0000000005ffe000 &&
                   BOOT_STACK_TOP(MMIX_MAX_CPUS - 1) ==
                     BOOT_STACK_AREA_BASE + BOOT_STACK_SIZE,
               "bootstrap stack geometry is invalid");
_Static_assert(BOOT_REGISTER_STACK_BASE(0) == REGISTER_STACK_BASE &&
                   BOOT_REGISTER_STACK_LIMIT(MMIX_MAX_CPUS - 1) <=
                     REGISTER_STACK_LIMIT,
               "initial register stacks exceed their reserved range");
_Static_assert(MMIX_CONTEXT_AREA_TOP == 0x0000080000000000 &&
                 MMIX_CONTEXT_AREA_BASE == 0x000007ffffce0000,
               "kernel context window must match the scheduler ABI");
_Static_assert(MMIX_CONTEXT_SLOT_STRIDE == 0xa000 &&
                 MMIX_CONTEXT_AREA_SIZE == 0x320000,
               "kernel context slot geometry must match the scheduler ABI");
_Static_assert(MMIX_CONTEXT_AREA_BASE > MMIO_BASE + INTC_SIZE,
               "kernel contexts must not overlap identity or device maps");
_Static_assert((MMIX_CONTEXT_AREA_BASE / 0x800000) ==
                 ((MMIX_CONTEXT_AREA_TOP - 1) / 0x800000),
               "kernel contexts must share one level-1 table span");
_Static_assert(MMIX_CONTEXT_HIGH_GUARD(MMIX_CONTEXT_SCHEDULER_SLOT(0)) ==
                   MMIX_CONTEXT_AREA_TOP - MMIX_PAGE_SIZE &&
                 MMIX_CONTEXT_HIGH_GUARD(
                   MMIX_CONTEXT_SCHEDULER_SLOT(MMIX_MAX_CPUS - 1)) ==
                   0x000007fffff68000 &&
                 MMIX_CONTEXT_LOW_GUARD(MMIX_CONTEXT_SLOT_COUNT - 1) ==
                   MMIX_CONTEXT_AREA_BASE,
               "kernel context endpoints must match the reserved window");
_Static_assert(KSTACK(0) == 0x000007fffff58000 &&
                 KSTACK(63) == 0x000007ffffce2000,
               "process kernel-stack endpoints must match the scheduler ABI");
_Static_assert(MMIX_USER_IMAGE_BASE == MMIX_PAGE_SIZE &&
                 MMIX_USER_LOW_GUARD_BASE == 0 &&
                 MMIX_USER_HEAP_LIMIT == MMIX_USER_STACK_GUARD_BASE &&
                 MMIX_USER_STACK_BASE ==
                   MMIX_USER_STACK_GUARD_BASE + MMIX_PAGE_SIZE &&
                 MMIX_USER_STACK_TOP == MMIX_USER_SEGMENT0_LIMIT &&
                 MMIX_USER_STACK_PAGES == 1,
               "user segment-0 layout must match the user ABI");
_Static_assert(MMIX_EXEC_ARG_MAX <=
                 MMIX_USER_STACK_PAGES * MMIX_PAGE_SIZE,
               "exec arguments must fit in the user software stack");
_Static_assert(((MMIX_USER_IMAGE_BASE | MMIX_USER_HEAP_LIMIT |
                  MMIX_USER_STACK_BASE | MMIX_USER_STACK_TOP) &
                 (MMIX_PAGE_SIZE - 1)) == 0,
               "user segment-0 boundaries must be page-aligned");
_Static_assert((MMIX_USER_REGISTER_STACK_BASE >> 61) == 3 &&
                 MMIX_USER_REGISTER_STACK_BASE ==
                   MMIX_USER_REGISTER_GUARD_BASE + MMIX_PAGE_SIZE &&
                 MMIX_USER_REGISTER_STACK_PAGES == 16 &&
                 MMIX_USER_REGISTER_GUARD_TOP ==
                   MMIX_USER_REGISTER_STACK_TOP + MMIX_PAGE_SIZE,
               "user register-stack layout must match the user ABI");
_Static_assert(KALLOC_LOW_LIMIT <= LOW_RAM_END,
               "Low allocator limit must remain inside Low RAM");
_Static_assert(KALLOC_START(KERNEL_LOAD) < KALLOC_LOW_LIMIT,
               "bootstrap allocator range must be non-empty");
_Static_assert(KALLOC_RECLAIMED_START == POOL_PHYS_BASE &&
                   KALLOC_RECLAIMED_LIMIT == STACK_PHYS_END &&
                   KALLOC_RECLAIMED_PAGES == 17408,
               "reclaimed allocator range must cover the bare segments");
#endif

#endif
