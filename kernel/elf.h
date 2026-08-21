#ifndef XV6_ELF_H
#define XV6_ELF_H

// Format of an ELF executable file

#define ELF_MAGIC 0x7f454c46U // "\x7FELF" in big endian

#define ELF_CLASS_64        2
#define ELF_DATA_BIG_ENDIAN 2
#define ELF_IDENT_VERSION   1
#define ELF_VERSION_CURRENT 1
#define ELF_TYPE_EXEC       2
#define ELF_MACHINE_MMIX    80

#define ELF_MAX_PROGRAM_HEADERS 16
#define ELF_MAX_LOAD_SEGMENTS   8

// File header.
struct elfhdr {
  uint magic; // must equal ELF_MAGIC
  uchar elf[12];
  ushort type;
  ushort machine;
  uint version;
  uint64 entry;
  uint64 phoff;
  uint64 shoff;
  uint flags;
  ushort ehsize;
  ushort phentsize;
  ushort phnum;
  ushort shentsize;
  ushort shnum;
  ushort shstrndx;
};

// Program header.
struct proghdr {
  uint32 type;
  uint32 flags;
  uint64 off;
  uint64 vaddr;
  uint64 paddr;
  uint64 filesz;
  uint64 memsz;
  uint64 align;
};

// Values for Proghdr type
#define ELF_PROG_NULL 0
#define ELF_PROG_LOAD 1

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC  1
#define ELF_PROG_FLAG_WRITE 2
#define ELF_PROG_FLAG_READ  4
#define ELF_PROG_FLAG_MASK                                                     \
  (ELF_PROG_FLAG_EXEC | ELF_PROG_FLAG_WRITE | ELF_PROG_FLAG_READ)

_Static_assert(sizeof(struct elfhdr) == 64, "ELF64 header size mismatch");
_Static_assert(sizeof(struct proghdr) == 56,
               "ELF64 program-header size mismatch");

#endif // XV6_ELF_H
