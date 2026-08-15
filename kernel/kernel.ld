OUTPUT_FORMAT(elf64-mmix)
OUTPUT_ARCH(mmix)
ENTRY(_entry)

SECTIONS
{
  . = 0x00100000;

  .text : ALIGN(0x2000) {
    KEEP(kernel/entry.o(.text))
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
  ASSERT((kernel_text_end & 0x1fff) == 0,
         "kernel text end is not page-aligned")
  ASSERT(kernel_text_end <= kernel_rodata_end,
         "kernel read-only data precedes text end")
  ASSERT((kernel_rodata_end & 0x1fff) == 0,
         "kernel read-only data end is not page-aligned")
  ASSERT(kernel_rodata_end <= kernel_end,
         "kernel writable data precedes read-only data end")
  ASSERT(kernel_end <= 0x05ffe000,
         "kernel overlaps the bootstrap stack")

  /DISCARD/ : {
    *(.comment)
    *(.note.GNU-stack)
  }
}
