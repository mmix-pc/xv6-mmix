OUTPUT_FORMAT(elf64-mmix)
OUTPUT_ARCH(mmix)
ENTRY(_entry)

SECTIONS
{
  . = 0x00100000;

  .text : ALIGN(0x2000) {
    KEEP(kernel/entry.o(.text))
    . = ALIGN(16);
    KEEP(kernel/kernelvec.o(.text))
    *(.text .text.*)
  }
  . = ALIGN(0x2000);
  kernel_text_end = .;

  .rodata : ALIGN(0x2000) {
    *(.rodata .rodata.*)
  }
  . = ALIGN(0x2000);
  kernel_rodata_end = .;

  .data : ALIGN(0x2000) {
    *(.data .data.*)
  }

  .bss : ALIGN(0x2000) {
    *(.bss .bss.*)
    *(COMMON)
  }

  . = ALIGN(0x2000);
  kernel_end = .;

  kernel_low_vectors_start = 0x00000000;
  kernel_low_vectors_end = 0x00002000;
  kernel_root_tables_start = 0x00002000;
  kernel_root_tables_end = 0x00008000;
  kernel_boot_stacks_start = 0x05fe0000;
  kernel_boot_stacks_end = 0x06000000;

  ASSERT(_entry == 0x00100000,
         "kernel entry does not match KERNEL_ENTRY")
  ASSERT(kernel_low_vectors_start == 0 &&
             kernel_low_vectors_end - kernel_low_vectors_start == 0x2000,
         "MMIX low vectors must reserve the first physical page")
  ASSERT(kernel_root_tables_start == kernel_low_vectors_end,
         "kernel root tables must follow the low-vector page")
  ASSERT((kernel_root_tables_start & 0x1fff) == 0,
         "kernel root tables are not page-aligned")
  ASSERT(kernel_root_tables_end - kernel_root_tables_start == 0x6000,
         "kernel root table reservation has the wrong size")
  ASSERT(kernel_root_tables_end <= 0x00010000,
         "kernel root tables overlap the register stack")
  ASSERT(kernel_text_end > 0x00100000,
         "kernel text is empty")
  ASSERT((mmix_kernel_trap_entry & 0xf) == 0 &&
             mmix_kernel_trap_entry >= 0x00100000 &&
             mmix_kernel_trap_entry < kernel_text_end,
         "MMIX kernel trap entry is misplaced")
  ASSERT((mmix_user_trap_entry & 0xf) == 0 &&
             mmix_user_trap_entry >= 0x00100000 &&
             mmix_user_trap_entry < kernel_text_end &&
             (mmix_user_resume & 0xf) == 0 &&
             mmix_user_resume >= 0x00100000 &&
             mmix_user_resume < kernel_text_end,
         "MMIX user transition code is misplaced")
  ASSERT((0x00100000 & 0xf) == 0 &&
             kernel_text_end < 0x8000000000000000,
         "kernel text cannot provide a negative MMIX trap alias")
  ASSERT((kernel_text_end & 0x1fff) == 0,
         "kernel text end is not page-aligned")
  ASSERT(kernel_text_end <= kernel_rodata_end,
         "kernel read-only data precedes text end")
  ASSERT((kernel_rodata_end & 0x1fff) == 0,
         "kernel read-only data end is not page-aligned")
  ASSERT(kernel_rodata_end <= kernel_end,
         "kernel writable data precedes read-only data end")
  ASSERT(cpus >= kernel_rodata_end &&
             cpus < kernel_end &&
             mmix_trap_vector >= kernel_rodata_end &&
             mmix_trap_vector + 8 <= kernel_end,
         "MMIX trap state is outside kernel writable storage")
  ASSERT(kernel_end <= kernel_boot_stacks_start,
         "kernel overlaps the bootstrap stacks")
  ASSERT(kernel_boot_stacks_end - kernel_boot_stacks_start == 0x20000 &&
             (kernel_boot_stacks_start & 0x1fff) == 0,
         "MMIX bootstrap stack reservation is invalid")
  ASSERT(kernel_boot_stacks_start + 1024 <
             kernel_boot_stacks_start + 0x2000,
         "a bootstrap stack cannot hold the MMIX trap path")
  ASSERT(0x00008000 <= 0x00010000 &&
             0x00010000 + 16 * 0x00008000 <= 0x00100000,
         "MMIX register-stack backing range is invalid")

  /DISCARD/ : {
    *(.comment)
    *(.note.GNU-stack)
  }
}
