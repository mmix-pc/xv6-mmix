OUTPUT_FORMAT(elf64-mmix)
OUTPUT_ARCH(mmix)
ENTRY(start)

PHDRS
{
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 0x2000;
  user_image_start = .;

  .text : ALIGN(0x2000) {
    user_text_start = .;
    *(.text .text.*)
    user_text_end = .;
  } :text

  . = ALIGN(0x2000);
  .rodata : {
    user_rodata_start = .;
    *(.rodata .rodata.*)
    user_rodata_end = .;
  } :rodata

  . = ALIGN(0x2000);
  .data : {
    user_data_start = .;
    *(.data .data.*)
    *(.got .got.*)
  } :data

  .bss : {
    *(.bss .bss.*)
    *(COMMON)
    user_data_end = .;
  } :data

  . = ALIGN(8);
  PROVIDE(end = .);
  user_image_end = .;

  /DISCARD/ : {
    *(.comment)
    *(.eh_frame .eh_frame.*)
    *(.note .note.*)
  }

  ASSERT(user_image_start == 0x2000, "MMIX user image overlaps low vectors")
  ASSERT(user_image_end <= 0x7f6000, "MMIX user image overlaps stack guard")
}
