#ifndef XV6_MMIX_MEMLAYOUT_H
#define XV6_MMIX_MEMLAYOUT_H

// The production topology has one boot CPU and up to sixteen configured CPUs.
#define MMIX_MAX_CPUS 16
#define BOOT_CPU_ID 0

// FIXME: Replace these legacy device constants when the drivers consume the
// FDT register and interrupt descriptions. Device offsets belong to drivers.
#define MMIO_BASE 0x0000000010000000

#define UART0_BASE 0x0000000010000000
#define UART0_SIZE 0x0000000000000100
#define UART0_IRQ 1

#define VIRTIO0_BASE 0x0000000010001000
#define VIRTIO0_SIZE 0x0000000000001000
#define VIRTIO0_IRQ 2048
#define VIRTIO_MMIO_COUNT 1

#define FRAMEBUFFER_CONTROL_BASE 0x0000000010002000
#define FRAMEBUFFER_CONTROL_SIZE 0x0000000000001000
// Reserved by the machine ABI; QEMU does not currently connect this source.
#define FRAMEBUFFER_IRQ 3

#define TIMER_BASE 0x0000000010003000
#define TIMER_SIZE 0x0000000000001000
#define TIMER_IRQ_BASE 16
#define TIMER_IRQ_COUNT_MAX MMIX_MAX_CPUS

#define IPI_BASE 0x0000000010006000
#define IPI_SIZE 0x0000000000001000
#define IPI_TARGET_COUNT_MAX MMIX_MAX_CPUS
#define IPI_REQUEST_MASK 0x0000000000000200

// The current kernel table construction uses the level-1 root directly.
#define KERNEL_IDENTITY_LIMIT 0x0000000200000000

// Bootstrap physical layout within contiguous RAM. The linker defines image
// boundaries; the ownership planner excludes all live reservations.
//
//   0x0000000000000000 +----------------------------------+
//                      | Reserved low-vector page (8 KiB) |
//   0x0000000000002000 +----------------------------------+
//                      | Kernel root tables (24 KiB)      |
//   0x0000000000008000 +----------------------------------+
//                      | Allocator-eligible RAM           |
//   0x0000000000100000 +----------------------------------+ KERNEL_LOAD
//                      | Kernel text, data, and BSS       |
//                      +----------------------------------+
//                      | Image-owned bootstrap stacks    |
//                      +----------------------------------+ kernel_end
//                      | RAM minus live FDT/stack/device  |
//                      | reservations discovered at boot  |
//      runtime RAM end +----------------------------------+
#define MMIX_PAGE_SIZE 0x0000000000002000
#define MMIX_PAGE_SHIFT 13

// The alignment must be a power of two.
#define ROUNDUP(value, alignment)                                             \
  (((value) + (alignment) - 1) & ~((alignment) - 1))
#define ROUNDDOWN(value, alignment) ((value) & ~((alignment) - 1))

// Reserve the first physical page for MMIX's fixed low TRIP vectors. The
// kernel does not map the corresponding virtual page until TRIP support exists.
#define MMIX_LOW_VECTOR_BASE 0
#define MMIX_LOW_VECTOR_SIZE MMIX_PAGE_SIZE
#define MMIX_LOW_VECTOR_LIMIT (MMIX_LOW_VECTOR_BASE + MMIX_LOW_VECTOR_SIZE)

// The kernel rV layout uses three contiguous physical root-table blocks in
// the existing low-address guard. Indirect child tables come from kalloc.
#define KERNEL_ROOT_BASE MMIX_LOW_VECTOR_LIMIT
#define KERNEL_ROOT_BLOCKS 3
#define KERNEL_ROOT_SIZE (KERNEL_ROOT_BLOCKS * MMIX_PAGE_SIZE)
#define KERNEL_ROOT_LIMIT (KERNEL_ROOT_BASE + KERNEL_ROOT_SIZE)

#define KERNEL_LOAD 0x0000000000100000
#define KERNEL_ENTRY KERNEL_LOAD
// Image ceiling matches the linker limit and the minimum 128-MiB RAM size.
#define KERNEL_LIMIT 0x0000000008000000

#define BOOT_STACK_SIZE MMIX_PAGE_SIZE
#define BOOT_STACK_COUNT MMIX_MAX_CPUS
#define BOOT_STACK_AREA_SIZE (BOOT_STACK_COUNT * BOOT_STACK_SIZE)

// Kernel virtual address map. Identity ranges map a virtual address to the
// same physical address. Negative addresses produced by mmix_phys_alias()
// provide privileged direct aliases of physical memory.
//
//   0x0000000000000000 +----------------------------------+
//                      | Unmapped vectors/root tables     |
//   0x0000000000008000 +----------------------------------+
//                      | Identity-mapped writable RAM     |
//   0x0000000000100000 +----------------------------------+ KERNEL_LOAD
//                      | Kernel text (read/execute)       |
//      kernel_text_end +----------------------------------+
//                      | Kernel rodata (read-only)        |
//    kernel_rodata_end +----------------------------------+
//                      | Identity-mapped writable RAM     |
//                      | Active FDT pages are read-only   |
//                      | Framebuffer pages are unmapped   |
//      runtime RAM end +----------------------------------+
//                      | Unmapped                         |
//   0x000007ffffce0000 +----------------------------------+
//                      | 80 kernel context slots          |
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

#if !defined(__ASSEMBLER__)
_Static_assert((MMIX_PAGE_SIZE & (MMIX_PAGE_SIZE - 1)) == 0,
               "MMIX page size must be a power of two");
_Static_assert((KERNEL_ROOT_BASE & (MMIX_PAGE_SIZE - 1)) == 0,
               "kernel root tables must be page-aligned");
_Static_assert(MMIX_LOW_VECTOR_BASE == 0 &&
                   MMIX_LOW_VECTOR_SIZE == MMIX_PAGE_SIZE,
               "MMIX low vectors must reserve the first physical page");
_Static_assert(KERNEL_ROOT_BASE == MMIX_LOW_VECTOR_LIMIT,
               "kernel root tables must follow the low-vector page");
_Static_assert(KERNEL_ROOT_SIZE == 0x6000,
               "kernel rV must reserve three root-table blocks");
_Static_assert(KERNEL_ROOT_LIMIT <= KERNEL_LOAD,
               "kernel root tables must remain below the kernel image");

_Static_assert(UART0_BASE >= MMIO_BASE &&
                   UART0_BASE + UART0_SIZE <= VIRTIO0_BASE,
               "UART MMIO range must not overlap VirtIO");
_Static_assert(VIRTIO0_BASE + VIRTIO0_SIZE <= FRAMEBUFFER_CONTROL_BASE,
               "VirtIO MMIO range must not overlap framebuffer control");
_Static_assert(FRAMEBUFFER_CONTROL_BASE + FRAMEBUFFER_CONTROL_SIZE <=
                   TIMER_BASE,
               "framebuffer control must not overlap the timer");
_Static_assert(TIMER_IRQ_COUNT_MAX == MMIX_MAX_CPUS &&
                   IPI_TARGET_COUNT_MAX == MMIX_MAX_CPUS,
               "CPU-local devices must cover the maximum topology");
_Static_assert((KERNEL_LOAD & (MMIX_PAGE_SIZE - 1)) == 0,
               "kernel load address must be page-aligned");
_Static_assert(MMIX_PAGE_SIZE == (1 << MMIX_PAGE_SHIFT),
               "MMIX page size and shift must agree");
_Static_assert(MMIX_MAX_CPUS <= 256,
               "CPU identities must fit in the rU usage-pattern byte");
_Static_assert(BOOT_STACK_SIZE == MMIX_PAGE_SIZE &&
                   BOOT_STACK_COUNT == MMIX_MAX_CPUS &&
                   BOOT_STACK_AREA_SIZE == 0x20000,
               "the kernel must reserve one bootstrap page per CPU");
_Static_assert(MMIX_CONTEXT_AREA_TOP == 0x0000080000000000 &&
                 MMIX_CONTEXT_AREA_BASE == 0x000007ffffce0000,
               "kernel context window must match the scheduler ABI");
_Static_assert(MMIX_CONTEXT_SLOT_STRIDE == 0xa000 &&
                 MMIX_CONTEXT_AREA_SIZE == 0x320000,
               "kernel context slot geometry must match the scheduler ABI");
_Static_assert(MMIX_CONTEXT_AREA_BASE >= KERNEL_IDENTITY_LIMIT,
               "kernel contexts must not overlap the RAM identity map");
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
#endif

#endif
