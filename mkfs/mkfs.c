#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define stat xv6_stat // avoid clash with host struct stat
#include "kernel/types.h"
#include "kernel/elf.h"
#include "kernel/fs.h"
#include "kernel/fsdisk.h"
#include "kernel/memlayout.h"
#include "kernel/param.h"
#include "kernel/stat.h"

enum {
  NINODES = 200,
  ELF_HEADER_BYTES = 64,
  ELF_PROGRAM_HEADER_BYTES = 56,
};

struct input {
  const char *path;
  const char *name;
  uint size;
};

// Disk layout:
// [ boot block | super block | log | inode blocks | bitmap | data blocks ]
static const uint nbitmap = (FSSIZE + BPB - 1) / BPB;
static const uint ninodeblocks = (NINODES + IPB - 1) / IPB;
static const uint nlog = LOGBLOCKS + 1;
static uint nmeta;
static uint nblocks;

static int fsfd = -1;
static struct superblock sb;
static uint freeinode = 1;
static uint freeblock;

static void balloc(uint);
static void die(const char *);
static uint ialloc(ushort);
static void iappend(uint, const void *, uint);
static void read_sector(uint, void *);
static void read_inode(uint, struct dinode *);
static void validate_inputs(int, char **, struct input *);
static void write_inode(uint, const struct dinode *);
static void write_sector(uint, const void *);

static ushort
load_be16(const uchar *p)
{
  return (ushort)p[0] << 8 | p[1];
}

static uint
load_be32(const uchar *p)
{
  return (uint)p[0] << 24 | (uint)p[1] << 16 | (uint)p[2] << 8 | p[3];
}

static uint64
load_be64(const uchar *p)
{
  return (uint64)load_be32(p) << 32 | load_be32(p + 4);
}

static void
fail_input(const char *path, const char *reason)
{
  fprintf(stderr, "mkfs: %s: %s\n", path, reason);
  exit(1);
}

static void
read_exact_at(int fd, void *destination, uint count, uint64 offset,
              const char *path)
{
  uchar *p = destination;

  if (offset > (uint64)((off_t)-1) ||
      lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset)
    die(path);
  while (count != 0) {
    ssize_t n = read(fd, p, count);

    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      fail_input(path, "truncated input");
    p += n;
    count -= (uint)n;
  }
}

static void
validate_user_elf(int fd, uint file_size, const char *path)
{
  uchar header[ELF_HEADER_BYTES];
  uint64 entry;
  uint64 phoff;
  uint64 segment_start[ELF_MAX_LOAD_SEGMENTS];
  uint64 segment_end[ELF_MAX_LOAD_SEGMENTS];
  uint load_count = 0;
  int entry_is_executable = 0;

  if (file_size < sizeof(header))
    fail_input(path, "user program is not an ELF64 executable");
  read_exact_at(fd, header, sizeof(header), 0, path);
  if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' ||
      header[3] != 'F' || header[4] != ELF_CLASS_64 ||
      header[5] != ELF_DATA_BIG_ENDIAN || header[6] != ELF_IDENT_VERSION ||
      header[7] != 0 || header[8] != 0 ||
      load_be16(header + 16) != ELF_TYPE_EXEC ||
      load_be16(header + 18) != ELF_MACHINE_MMIX ||
      load_be32(header + 20) != ELF_VERSION_CURRENT ||
      load_be32(header + 48) != 0 ||
      load_be16(header + 52) != ELF_HEADER_BYTES ||
      load_be16(header + 54) != ELF_PROGRAM_HEADER_BYTES ||
      load_be16(header + 56) == 0 ||
      load_be16(header + 56) > ELF_MAX_PROGRAM_HEADERS)
    fail_input(path, "user program violates the MMIX ELF header contract");
  for (uint i = 9; i < 16; i++)
    if (header[i] != 0)
      fail_input(path, "user program has nonzero ELF identification padding");

  entry = load_be64(header + 24);
  phoff = load_be64(header + 32);
  uint phnum = load_be16(header + 56);
  uint64 table_size = (uint64)phnum * ELF_PROGRAM_HEADER_BYTES;
  if (phoff < ELF_HEADER_BYTES || phoff > file_size ||
      table_size > file_size - phoff)
    fail_input(path, "user program has an invalid program-header table");

  for (uint index = 0; index < phnum; index++) {
    uchar ph[ELF_PROGRAM_HEADER_BYTES];
    uint type;
    uint flags;
    uint64 offset;
    uint64 vaddr;
    uint64 paddr;
    uint64 filesz;
    uint64 memsz;
    uint64 align;
    uint64 page_end;

    read_exact_at(fd, ph, sizeof(ph), phoff + index * sizeof(ph), path);
    type = load_be32(ph + 0);
    if (type == ELF_PROG_NULL)
      continue;
    if (type != ELF_PROG_LOAD || load_count == ELF_MAX_LOAD_SEGMENTS)
      fail_input(path, "user program has an unsupported program header");
    flags = load_be32(ph + 4);
    offset = load_be64(ph + 8);
    vaddr = load_be64(ph + 16);
    paddr = load_be64(ph + 24);
    filesz = load_be64(ph + 32);
    memsz = load_be64(ph + 40);
    align = load_be64(ph + 48);
    if (memsz == 0 || filesz > memsz ||
        (flags & ~ELF_PROG_FLAG_MASK) != 0 ||
        (flags & ELF_PROG_FLAG_READ) == 0 ||
        (flags & (ELF_PROG_FLAG_WRITE | ELF_PROG_FLAG_EXEC)) ==
          (ELF_PROG_FLAG_WRITE | ELF_PROG_FLAG_EXEC) ||
        align != MMIX_PAGE_SIZE || (vaddr & (MMIX_PAGE_SIZE - 1)) != 0 ||
        (offset & (MMIX_PAGE_SIZE - 1)) != 0 || paddr != vaddr ||
        vaddr < MMIX_USER_IMAGE_BASE || vaddr >= MMIX_USER_HEAP_LIMIT ||
        memsz > MMIX_USER_HEAP_LIMIT - vaddr || offset > file_size ||
        filesz > file_size - offset)
      fail_input(path, "user program has an invalid load segment");
    page_end = (vaddr + memsz + MMIX_PAGE_SIZE - 1) &
               ~(MMIX_PAGE_SIZE - 1);
    for (uint prior = 0; prior < load_count; prior++)
      if (vaddr < segment_end[prior] && segment_start[prior] < page_end)
        fail_input(path, "user program has overlapping load segments");
    segment_start[load_count] = vaddr;
    segment_end[load_count++] = page_end;
    if (entry >= vaddr && entry < vaddr + filesz &&
        (flags & ELF_PROG_FLAG_EXEC) != 0)
      entry_is_executable = 1;
  }
  if (load_count == 0 || !entry_is_executable)
    fail_input(path, "user program has no executable entry segment");
}

static int
is_user_program(const char *path)
{
  if (strncmp(path, "user/", 5) == 0)
    path += 5;
  return path[0] == '_';
}

static const char *
image_name(const char *path)
{
  const char *name = path;

  if (strncmp(path, "user/", 5) == 0)
    name = path + 5;
  if (strchr(name, '/') != 0 || name[0] == '\0')
    fail_input(path, "input must be in the current or user directory");
  if (name[0] == '_')
    name++;
  if (name[0] == '\0' || strlen(name) > DIRSIZ ||
      strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    fail_input(path, "invalid file-system name");
  return name;
}

static void
validate_inputs(int count, char **paths, struct input *inputs)
{
  uint root_bytes = (uint)(count + 2) * XV6FS_DIRENT_BYTES;
  uint required_data_blocks = (root_bytes + BSIZE - 1) / BSIZE;

  if ((uint)count + ROOTINO >= NINODES)
    fail_input(paths[0], "too many input files");
  if ((uint)(count + 2) * XV6FS_DIRENT_BYTES > MAXFILE * BSIZE)
    fail_input(paths[0], "root directory is too large");

  for (int i = 0; i < count; i++) {
    const char *name = image_name(paths[i]);
    int fd = open(paths[i], O_RDONLY);
    off_t size;

    if (fd < 0)
      die(paths[i]);
    size = lseek(fd, 0, SEEK_END);
    if (size < 0)
      die(paths[i]);
    if ((uint64)size > (uint64)MAXFILE * BSIZE)
      fail_input(paths[i], "file exceeds the xv6 maximum file size");
    if (is_user_program(paths[i]))
      validate_user_elf(fd, (uint)size, paths[i]);
    for (int prior = 0; prior < i; prior++)
      if (strcmp(name, inputs[prior].name) == 0)
        fail_input(paths[i], "duplicate file-system name");
    inputs[i].path = paths[i];
    inputs[i].name = name;
    inputs[i].size = (uint)size;
    uint file_blocks = ((uint)size + BSIZE - 1) / BSIZE;
    required_data_blocks += file_blocks;
    if (file_blocks > NDIRECT)
      required_data_blocks++;
    if (close(fd) < 0)
      die(paths[i]);
  }
  if (required_data_blocks > nblocks)
    fail_input(paths[0], "input files do not fit in the file system");
}

int
main(int argc, char *argv[])
{
  uchar block[BSIZE];
  struct input *inputs;
  uint rootino;

  _Static_assert(sizeof(int) == 4, "host int must be four bytes");
  _Static_assert(IPB * XV6FS_DINODE_BYTES == BSIZE,
                 "inode block geometry mismatch");
  _Static_assert(XV6FS_DIRENT_BYTES == sizeof(ushort) + DIRSIZ,
                 "directory entry geometry mismatch");

  if (argc < 2) {
    fprintf(stderr, "usage: mkfs fs.img files...\n");
    return 1;
  }

  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;
  sb.magic = FSMAGIC;
  sb.size = FSSIZE;
  sb.nblocks = nblocks;
  sb.ninodes = NINODES;
  sb.nlog = nlog;
  sb.logstart = 2;
  sb.inodestart = sb.logstart + nlog;
  sb.bmapstart = sb.inodestart + ninodeblocks;
  freeblock = nmeta;

  inputs = calloc(argc > 2 ? (size_t)(argc - 2) : 1, sizeof(*inputs));
  if (inputs == 0)
    die("calloc");
  validate_inputs(argc - 2, argv + 2, inputs);

  fsfd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (fsfd < 0)
    die(argv[1]);
  memset(block, 0, sizeof(block));
  for (uint sector = 0; sector < FSSIZE; sector++)
    write_sector(sector, block);
  xv6fs_store_le32(block + 0, sb.magic);
  xv6fs_store_le32(block + 4, sb.size);
  xv6fs_store_le32(block + 8, sb.nblocks);
  xv6fs_store_le32(block + 12, sb.ninodes);
  xv6fs_store_le32(block + 16, sb.nlog);
  xv6fs_store_le32(block + 20, sb.logstart);
  xv6fs_store_le32(block + 24, sb.inodestart);
  xv6fs_store_le32(block + 28, sb.bmapstart);
  write_sector(1, block);

  printf("nmeta %u (boot, super, log blocks %u, inode blocks %u, bitmap "
         "blocks %u) blocks %u total %u\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  rootino = ialloc(T_DIR);
  if (rootino != ROOTINO)
    fail_input(argv[1], "root inode allocation failed");

  uchar entry[XV6FS_DIRENT_BYTES];
  struct dirent de;
  memset(&de, 0, sizeof(de));
  de.inum = rootino;
  strcpy(de.name, ".");
  xv6fs_dirent_encode(entry, &de);
  iappend(rootino, entry, sizeof(entry));
  memset(&de, 0, sizeof(de));
  de.inum = rootino;
  strcpy(de.name, "..");
  xv6fs_dirent_encode(entry, &de);
  iappend(rootino, entry, sizeof(entry));

  for (int i = 0; i < argc - 2; i++) {
    int fd = open(inputs[i].path, O_RDONLY);
    uint inum;
    uint written = 0;

    if (fd < 0)
      die(inputs[i].path);
    inum = ialloc(T_FILE);
    memset(&de, 0, sizeof(de));
    de.inum = inum;
    memcpy(de.name, inputs[i].name, strlen(inputs[i].name));
    xv6fs_dirent_encode(entry, &de);
    iappend(rootino, entry, sizeof(entry));

    for (;;) {
      ssize_t count = read(fd, block, sizeof(block));

      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0)
        die(inputs[i].path);
      if (count == 0)
        break;
      iappend(inum, block, (uint)count);
      written += (uint)count;
    }
    if (written != inputs[i].size)
      fail_input(inputs[i].path, "input changed while building the image");
    if (close(fd) < 0)
      die(inputs[i].path);
  }

  struct dinode root;
  read_inode(rootino, &root);
  root.size = (root.size + BSIZE - 1) / BSIZE * BSIZE;
  write_inode(rootino, &root);
  balloc(freeblock);

  if (close(fsfd) < 0)
    die(argv[1]);
  free(inputs);
  return 0;
}

static void
write_sector(uint sector, const void *buffer)
{
  const uchar *p = buffer;
  uint remaining = BSIZE;
  off_t offset = (off_t)sector * BSIZE;

  if (sector >= FSSIZE || lseek(fsfd, offset, SEEK_SET) != offset)
    die("image seek");
  while (remaining != 0) {
    ssize_t count = write(fsfd, p, remaining);

    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      die("image write");
    p += count;
    remaining -= (uint)count;
  }
}

static void
read_sector(uint sector, void *buffer)
{
  uchar *p = buffer;
  uint remaining = BSIZE;
  off_t offset = (off_t)sector * BSIZE;

  if (sector >= FSSIZE || lseek(fsfd, offset, SEEK_SET) != offset)
    die("image seek");
  while (remaining != 0) {
    ssize_t count = read(fsfd, p, remaining);

    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      die("image read");
    p += count;
    remaining -= (uint)count;
  }
}

static void
write_inode(uint inum, const struct dinode *inode)
{
  uchar block[BSIZE];
  uint sector = inum / IPB + sb.inodestart;

  if (inum >= sb.ninodes)
    fail_input("image", "inode number out of range");
  read_sector(sector, block);
  xv6fs_dinode_encode(block + (inum % IPB) * XV6FS_DINODE_BYTES, inode);
  write_sector(sector, block);
}

static void
read_inode(uint inum, struct dinode *inode)
{
  uchar block[BSIZE];
  uint sector = inum / IPB + sb.inodestart;

  if (inum >= sb.ninodes)
    fail_input("image", "inode number out of range");
  read_sector(sector, block);
  xv6fs_dinode_decode(inode,
                      block + (inum % IPB) * XV6FS_DINODE_BYTES);
}

static uint
ialloc(ushort type)
{
  struct dinode inode;
  uint inum = freeinode++;

  if (inum >= sb.ninodes)
    fail_input("image", "out of inodes");
  memset(&inode, 0, sizeof(inode));
  inode.type = type;
  inode.nlink = 1;
  write_inode(inum, &inode);
  return inum;
}

static void
balloc(uint used)
{
  uchar block[BSIZE];

  if (used > FSSIZE || nbitmap != 1)
    fail_input("image", "bitmap geometry is unsupported");
  memset(block, 0, sizeof(block));
  for (uint i = 0; i < used; i++)
    block[i / 8] |= 1U << (i % 8);
  write_sector(sb.bmapstart, block);
  printf("balloc: first %u blocks have been allocated\n", used);
}

static void
iappend(uint inum, const void *source, uint count)
{
  const uchar *p = source;
  struct dinode inode;
  uchar block[BSIZE];
  uint offset;

  read_inode(inum, &inode);
  offset = inode.size;
  if (count > MAXFILE * BSIZE - offset)
    fail_input("image", "file exceeds maximum size while appending");
  while (count != 0) {
    uint file_block = offset / BSIZE;
    uint disk_block;

    if (file_block < NDIRECT) {
      if (inode.addrs[file_block] == 0)
        inode.addrs[file_block] = freeblock++;
      disk_block = inode.addrs[file_block];
    } else {
      uint indirect_index = file_block - NDIRECT;

      if (inode.addrs[NDIRECT] == 0)
        inode.addrs[NDIRECT] = freeblock++;
      read_sector(inode.addrs[NDIRECT], block);
      disk_block = xv6fs_load_le32(block + indirect_index * sizeof(uint));
      if (disk_block == 0) {
        disk_block = freeblock++;
        xv6fs_store_le32(block + indirect_index * sizeof(uint), disk_block);
        write_sector(inode.addrs[NDIRECT], block);
      }
    }
    if (freeblock > FSSIZE || disk_block < nmeta || disk_block >= FSSIZE)
      fail_input("image", "data block allocation is out of range");
    uint chunk = BSIZE - offset % BSIZE;
    if (chunk > count)
      chunk = count;
    read_sector(disk_block, block);
    memcpy(block + offset % BSIZE, p, chunk);
    write_sector(disk_block, block);
    offset += chunk;
    p += chunk;
    count -= chunk;
  }
  inode.size = offset;
  write_inode(inum, &inode);
}

static void
die(const char *what)
{
  fprintf(stderr, "mkfs: %s: %s\n", what, strerror(errno));
  exit(1);
}
