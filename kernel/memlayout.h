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
//   0x0000000010000000 +----------------------------------+
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
//                      | Extended RAM                     |
//   0x0000000020000000 +----------------------------------+ managed RAM end
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

#define PLATFORM_RAM_BASE 0x000000000e800000
#define PLATFORM_RAM_SIZE 0x0000000000800000
#define PLATFORM_RAM_END (PLATFORM_RAM_BASE + PLATFORM_RAM_SIZE)

#define BOOTINFO_BASE 0x000000000e800000
#define BOOTINFO_SIZE 0x0000000000000130
#define BOOTINFO_END (BOOTINFO_BASE + BOOTINFO_SIZE)

#define FRAMEBUFFER_BASE 0x000000000f000000
#define FRAMEBUFFER_SIZE 0x0000000001000000
#define FRAMEBUFFER_END (FRAMEBUFFER_BASE + FRAMEBUFFER_SIZE)

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
#define TIMER_IRQ_COUNT 1

#define INTC_BASE 0x0000000010004000
#define INTC_SIZE 0x0000000000002000
#define INTC_IRQ_COUNT 32
#define INTC_SHARED_IRQ_FIRST 1
#define INTC_SHARED_IRQ_LAST 15
#define INTC_CONTEXT_COUNT 16

// The kernel requires 512 MiB of machine RAM but deliberately manages no
// memory beyond this fixed limit. The MMIO pages below EXTENDED_RAM_BASE
// remain reserved even though QEMU exposes RAM underneath gaps between devices.
#define RAM_REQUIRED_SIZE 0x0000000020000000
#define RAM_MANAGED_END (LOW_RAM_BASE + RAM_REQUIRED_SIZE)
#define EXTENDED_RAM_BASE (INTC_BASE + INTC_SIZE)
#define EXTENDED_RAM_END RAM_MANAGED_END

// The current kernel configuration is single-CPU.
#define BOOT_CPU_COUNT 1
#define BOOT_CPU_ID 0

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
//                      | Bootstrap register stack         |
//   0x0000000000100000 +----------------------------------+ KERNEL_LOAD
//                      | Kernel image                     |
//                      +----------------------------------+ KALLOC_START(end)
//                      | Free pages below the boot stack  |
//   0x0000000005ffe000 +----------------------------------+ KERNEL_LIMIT
//                      | Bootstrap stack (8 KiB)          |
//   0x0000000006000000 +----------------------------------+
#define MMIX_PAGE_SIZE 0x0000000000002000

// The alignment must be a power of two.
#define ROUNDUP(value, alignment)                                             \
  (((value) + (alignment) - 1) & ~((alignment) - 1))
#define ROUNDDOWN(value, alignment) ((value) & ~((alignment) - 1))

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

#define KERNEL_LOAD 0x0000000000100000
#define KERNEL_ENTRY KERNEL_LOAD
#define KERNEL_LIMIT 0x0000000005ffe000

#define BOOT_STACK_BASE 0x0000000005ffe000
#define BOOT_STACK_SIZE MMIX_PAGE_SIZE
#define BOOT_STACK_TOP (BOOT_STACK_BASE + BOOT_STACK_SIZE)

// Kernel virtual address map. Identity ranges map a virtual address to the
// same physical address. Negative addresses produced by mmix_phys_alias()
// provide privileged direct aliases of physical memory.
//
//   0x0000000000000000 +----------------------------------+
//                      | Unmapped low vectors/tables/gap  |
//   0x0000000000010000 +----------------------------------+
//                      | Identity Low RAM                 |
//   0x0000000006000000 +----------------------------------+
//                      | Unmapped reserved physical area  |
//   0x0000000010000000 +----------------------------------+
//                      | Identity MMIO and Extended RAM   |
//   0x0000000020000000 +----------------------------------+
//                      | Unmapped                         |
//   0x000007ffffd76000 +----------------------------------+
//                      | 65 kernel context slots          |
//   0x0000080000000000 +----------------------------------+
//                      | Unmapped                         |
//   0x8000000000000000 +----------------------------------+
//                      | Direct aliases of managed memory |
//   0x8000000020000000 +----------------------------------+
//
// Slot 0 belongs to the scheduler; slots 1 through 64 correspond to
// proc[0..63]. Each slot has two mapped pages separated and bounded by guards.
#define MMIX_CONTEXT_AREA_TOP          0x0000080000000000
#define MMIX_CONTEXT_SLOT_COUNT        65
#define MMIX_CONTEXT_SCHEDULER_SLOT    0
#define MMIX_CONTEXT_PROCESS_SLOT_BASE 1
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
#define KALLOC_LOW_LIMIT BOOT_STACK_BASE
#define KALLOC_EXTENDED_START EXTENDED_RAM_BASE
#define KALLOC_EXTENDED_LIMIT EXTENDED_RAM_END

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
_Static_assert(INTC_BASE + INTC_SIZE == EXTENDED_RAM_BASE,
               "extended RAM must follow the MMIO envelope");
_Static_assert((RAM_REQUIRED_SIZE & (MMIX_PAGE_SIZE - 1)) == 0 &&
                   RAM_MANAGED_END == 0x0000000020000000,
               "the kernel must manage exactly 512 MiB of machine RAM");
_Static_assert((EXTENDED_RAM_BASE & (MMIX_PAGE_SIZE - 1)) == 0 &&
                   EXTENDED_RAM_BASE < EXTENDED_RAM_END,
               "extended RAM must be a non-empty page-aligned interval");

_Static_assert((KERNEL_LOAD & (MMIX_PAGE_SIZE - 1)) == 0,
               "kernel load address must be page-aligned");
_Static_assert((REGISTER_STACK_BASE & 7) == 0 &&
                   REGISTER_STACK_BASE < REGISTER_STACK_LIMIT,
               "register-stack backing range must be valid and octa-aligned");
_Static_assert(KERNEL_LOAD == REGISTER_STACK_LIMIT,
               "kernel must follow the reserved register-stack range");
_Static_assert(KERNEL_LIMIT == KALLOC_LOW_LIMIT &&
                   KALLOC_LOW_LIMIT == BOOT_STACK_BASE,
               "kernel limit must stop at the bootstrap stack");
_Static_assert(BOOT_STACK_SIZE == MMIX_PAGE_SIZE,
               "the kernel must reserve exactly one bootstrap stack page");
_Static_assert(BOOT_STACK_TOP == LOW_RAM_END,
               "bootstrap stack must end at the top of Low RAM");
_Static_assert(MMIX_CONTEXT_AREA_TOP == 0x0000080000000000 &&
                 MMIX_CONTEXT_AREA_BASE == 0x000007ffffd76000,
               "kernel context window must match the scheduler ABI");
_Static_assert(MMIX_CONTEXT_SLOT_STRIDE == 0xa000 &&
                 MMIX_CONTEXT_AREA_SIZE == 0x28a000,
               "kernel context slot geometry must match the scheduler ABI");
_Static_assert(MMIX_CONTEXT_AREA_BASE > MMIO_BASE + INTC_SIZE,
               "kernel contexts must not overlap identity or device maps");
_Static_assert((MMIX_CONTEXT_AREA_BASE / 0x800000) ==
                 ((MMIX_CONTEXT_AREA_TOP - 1) / 0x800000),
               "kernel contexts must share one level-1 table span");
_Static_assert(MMIX_CONTEXT_HIGH_GUARD(MMIX_CONTEXT_SCHEDULER_SLOT) ==
                   MMIX_CONTEXT_AREA_TOP - MMIX_PAGE_SIZE &&
                 MMIX_CONTEXT_LOW_GUARD(MMIX_CONTEXT_SLOT_COUNT - 1) ==
                   MMIX_CONTEXT_AREA_BASE,
               "kernel context endpoints must match the reserved window");
_Static_assert(KSTACK(0) == 0x000007fffffee000 &&
                 KSTACK(63) == 0x000007ffffd78000,
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
_Static_assert(KALLOC_EXTENDED_START == EXTENDED_RAM_BASE &&
                   KALLOC_EXTENDED_LIMIT == RAM_MANAGED_END,
               "extended allocator range must match the managed RAM tail");
#endif

#endif
