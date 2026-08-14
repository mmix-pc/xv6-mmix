#ifndef XV6_MMIX_MEMLAYOUT_H
#define XV6_MMIX_MEMLAYOUT_H

// QEMU MMIX virt physical memory map. Intervals are half-open.
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

// The current kernel configuration is single-CPU.
#define BOOT_CPU_COUNT 1
#define BOOT_CPU_ID 0

// Bootstrap physical layout within Low RAM.
#define MMIX_PAGE_SIZE 0x0000000000002000

// The kernel rV layout uses three contiguous physical root-table blocks in
// the existing low-address guard. Indirect child tables come from kalloc.
#define KERNEL_ROOT_BASE LOW_RAM_BASE
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

// FIXME: kernel.ld provides a page-aligned kernel_end. Keep this macro as an
// identity operation for now: the MMIX code generator otherwise folds the
// usual round-up addition into an unaligned GETA symbol addend that the linker
// cannot encode.
#define KALLOC_START(kernel_end) (kernel_end)
#define KALLOC_LIMIT BOOT_STACK_BASE

#if !defined(__ASSEMBLER__)
_Static_assert((MMIX_PAGE_SIZE & (MMIX_PAGE_SIZE - 1)) == 0,
               "MMIX page size must be a power of two");
_Static_assert((KERNEL_ROOT_BASE & (MMIX_PAGE_SIZE - 1)) == 0,
               "kernel root tables must be page-aligned");
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

_Static_assert((KERNEL_LOAD & (MMIX_PAGE_SIZE - 1)) == 0,
               "kernel load address must be page-aligned");
_Static_assert((REGISTER_STACK_BASE & 7) == 0 &&
                   REGISTER_STACK_BASE < REGISTER_STACK_LIMIT,
               "register-stack backing range must be valid and octa-aligned");
_Static_assert(KERNEL_LOAD == REGISTER_STACK_LIMIT,
               "kernel must follow the reserved register-stack range");
_Static_assert(KERNEL_LIMIT == BOOT_STACK_BASE,
               "kernel limit must stop at the bootstrap stack");
_Static_assert(BOOT_STACK_SIZE == MMIX_PAGE_SIZE,
               "the kernel must reserve exactly one bootstrap stack page");
_Static_assert(BOOT_STACK_TOP == LOW_RAM_END,
               "bootstrap stack must end at the top of Low RAM");
_Static_assert(KALLOC_LIMIT <= LOW_RAM_END,
               "allocator limit must remain inside Low RAM");
_Static_assert(KALLOC_START(KERNEL_LOAD) < KALLOC_LIMIT,
               "bootstrap allocator range must be non-empty");
#endif

#endif
