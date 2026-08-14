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

  .rodata : ALIGN(0x2000) {
    *(.rodata .rodata.*)
  }

  .data : ALIGN(0x2000) {
    *(.data .data.*)
  }

  .bss : ALIGN(0x2000) {
    *(.bss .bss.*)
    *(COMMON)
  }

  . = ALIGN(0x2000);
  kernel_end = .;

  ASSERT(_entry == 0x00100000,
         "kernel entry does not match KERNEL_ENTRY")
  ASSERT(kernel_end <= 0x05ffe000,
         "kernel overlaps the bootstrap stack")

  /DISCARD/ : {
    *(.comment)
    *(.note.GNU-stack)
  }
}
