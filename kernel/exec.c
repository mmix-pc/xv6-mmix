#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmix.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

static int
read_exact(struct inode *ip, uint64 dst, uint64 offset, uint size)
{
  if (offset > ip->size || size > ip->size - offset)
    return -1;
  return readi(ip, 0, dst, (uint)offset, size) == size ? 0 : -1;
}

static int
elf_header_valid(const struct elfhdr *elf, uint file_size)
{
  uint64 table_size;

  if (elf->magic != ELF_MAGIC || elf->elf[0] != ELF_CLASS_64 ||
      elf->elf[1] != ELF_DATA_BIG_ENDIAN || elf->elf[2] != ELF_IDENT_VERSION ||
      elf->elf[3] != 0 || elf->elf[4] != 0 || elf->type != ELF_TYPE_EXEC ||
      elf->machine != ELF_MACHINE_MMIX || elf->version != ELF_VERSION_CURRENT ||
      elf->flags != 0 || elf->ehsize != sizeof(*elf) ||
      elf->phentsize != sizeof(struct proghdr) || elf->phnum == 0 ||
      elf->phnum > ELF_MAX_PROGRAM_HEADERS || elf->phoff < sizeof(*elf))
    return 0;
  for (uint index = 5; index < sizeof(elf->elf); index++)
    if (elf->elf[index] != 0)
      return 0;
  table_size = (uint64)elf->phnum * sizeof(struct proghdr);
  return elf->phoff <= file_size && table_size <= file_size - elf->phoff;
}

static int
elf_segment_valid(const struct proghdr *ph, uint file_size)
{
  uint64 end;

  if (ph->type != ELF_PROG_LOAD || ph->memsz == 0 || ph->filesz > ph->memsz ||
      (ph->flags & ~ELF_PROG_FLAG_MASK) != 0 ||
      (ph->flags & ELF_PROG_FLAG_READ) == 0 ||
      (ph->flags & (ELF_PROG_FLAG_WRITE | ELF_PROG_FLAG_EXEC)) ==
        (ELF_PROG_FLAG_WRITE | ELF_PROG_FLAG_EXEC) ||
      ph->align != PGSIZE || (ph->vaddr & (PGSIZE - 1)) != 0 ||
      (ph->off & (PGSIZE - 1)) != 0 || ph->paddr != ph->vaddr ||
      ph->vaddr < MMIX_USER_IMAGE_BASE || ph->vaddr >= MMIX_USER_HEAP_LIMIT ||
      ph->memsz > MMIX_USER_HEAP_LIMIT - ph->vaddr)
    return 0;
  end = ph->vaddr + ph->memsz;
  if (end <= ph->vaddr || ph->off > file_size ||
      ph->filesz > file_size - ph->off)
    return 0;
  return 1;
}

// Map ELF permissions to PTE permission bits.
static int
flags2perm(uint flags)
{
  int permissions = PTE_R;

  if ((flags & ELF_PROG_FLAG_WRITE) != 0)
    permissions |= PTE_W;
  if ((flags & ELF_PROG_FLAG_EXEC) != 0)
    permissions |= PTE_X;
  return permissions;
}

// Load one file-backed part of an already mapped ELF segment. va must be
// page-aligned, and the pages covering size bytes must already be mapped.
// Allocation has zeroed the remainder of the final page and every BSS-only
// page. Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint64 offset,
        uint64 size)
{
  for (uint64 done = 0; done < size; done += PGSIZE) {
    uint64 pa = walkaddr(pagetable, va + done);
    uint n = PGSIZE;

    if (pa == 0)
      panic("loadseg mapping");
    if (size - done < PGSIZE)
      n = (uint)(size - done);
    if (read_exact(ip, pa, offset + done, n) < 0)
      return -1;
  }
  return 0;
}

// The implementation of the exec() system call.
int
kexec(char *path, char **argv)
{
  struct proghdr loads[ELF_MAX_LOAD_SEGMENTS];
  uint64 ustack[MAXARG + 1];
  struct elfhdr elf;
  struct inode *ip = 0;
  pagetable_t pagetable = 0;
  struct proc *p = myproc();
  uint64 image_end = MMIX_USER_IMAGE_BASE;
  uint64 sp = MMIX_USER_STACK_TOP;
  uint64 argv_address;
  uint argc;
  uint load_count = 0;
  char *last;
  char *s;

  if (p == 0)
    return -1;
  begin_op();

  // Open the executable file.
  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);

  // Read and validate the ELF header.
  if (read_exact(ip, (uint64)&elf, 0, sizeof(elf)) < 0 ||
      !elf_header_valid(&elf, ip->size))
    goto bad;

  // Read and validate the program headers.
  for (uint index = 0; index < elf.phnum; index++) {
    struct proghdr ph;
    uint64 offset = elf.phoff + (uint64)index * sizeof(ph);

    if (read_exact(ip, (uint64)&ph, offset, sizeof(ph)) < 0)
      goto bad;
    if (ph.type == ELF_PROG_NULL)
      continue;
    if (!elf_segment_valid(&ph, ip->size) ||
        load_count == ELF_MAX_LOAD_SEGMENTS)
      goto bad;
    uint64 page_end = PGROUNDUP(ph.vaddr + ph.memsz);
    for (uint prior = 0; prior < load_count; prior++) {
      uint64 prior_end = PGROUNDUP(loads[prior].vaddr + loads[prior].memsz);

      if (ph.vaddr < prior_end && loads[prior].vaddr < page_end)
        goto bad;
    }
    loads[load_count++] = ph;
  }
  if (load_count == 0)
    goto bad;

  int entry_is_executable = 0;
  for (uint index = 0; index < load_count; index++) {
    struct proghdr *ph = &loads[index];
    uint64 end = ph->vaddr + ph->memsz;

    if (elf.entry >= ph->vaddr && elf.entry < ph->vaddr + ph->filesz &&
        (ph->flags & ELF_PROG_FLAG_EXEC) != 0)
      entry_is_executable = 1;
    if (end > image_end)
      image_end = end;
  }
  if (!entry_is_executable)
    goto bad;

  if ((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load the program into memory.
  for (uint index = 0; index < load_count; index++) {
    struct proghdr *ph = &loads[index];
    uint64 end = ph->vaddr + ph->memsz;

    if (uvmalloc(pagetable, ph->vaddr, end, flags2perm(ph->flags)) == 0 ||
        loadseg(pagetable, ph->vaddr, ip, ph->off, ph->filesz) < 0)
      goto bad;
  }

  // Allocate the fixed MMIX user stack and its guard page.
  if (uvmallocstacks(pagetable) < 0)
    goto bad;

  iunlockput(ip);
  end_op();
  ip = 0;

  // Copy argument strings into the new stack and remember their addresses in
  // ustack[].
  for (argc = 0; argv[argc] != 0; argc++) {
    uint64 length;

    if (argc == MAXARG)
      goto bad;
    length = strlen(argv[argc]) + 1;
    if (length > sp - MMIX_USER_STACK_BASE)
      goto bad;
    sp = (sp - length) & ~(sizeof(uint64) - 1);
    if (copyout(pagetable, sp, argv[argc], length) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // Push a copy of ustack[], the array of argv[] pointers.
  uint64 array_size = (argc + 1) * sizeof(uint64);
  if (array_size > sp - MMIX_USER_STACK_BASE)
    goto bad;
  sp = (sp - array_size) & ~(sizeof(uint64) - 1);
  argv_address = sp;
  if (copyout(pagetable, argv_address, (char *)ustack, array_size) < 0)
    goto bad;

  // Commit to the new user image.
  if (proc_exec(pagetable, image_end, elf.entry, sp, argc, argv_address) < 0)
    goto bad;
  pagetable = 0;

  // Save the program name for debugging.
  for (last = s = path; *s != 0; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  return argc; // The first argument to MMIX start(argc, argv).

bad:
  if (pagetable != 0)
    proc_freepagetable(pagetable, image_end);
  if (ip != 0) {
    iunlockput(ip);
    end_op();
  }
  return -1;
}
