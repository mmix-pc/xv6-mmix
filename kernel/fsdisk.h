#ifndef XV6_FSDISK_H
#define XV6_FSDISK_H

#include "types.h"
#include "fs.h"

enum {
  XV6FS_SUPER_BYTES = 32,
  XV6FS_DINODE_BYTES = 64,
  XV6FS_DIRENT_BYTES = 16,
};

static inline ushort
xv6fs_load_le16(const void *storage)
{
  const uchar *p = storage;

  return (ushort)p[0] | (ushort)p[1] << 8;
}

static inline uint
xv6fs_load_le32(const void *storage)
{
  const uchar *p = storage;

  return (uint)p[0] | (uint)p[1] << 8 | (uint)p[2] << 16 |
         (uint)p[3] << 24;
}

static inline void
xv6fs_store_le16(void *storage, ushort value)
{
  uchar *p = storage;

  p[0] = value;
  p[1] = value >> 8;
}

static inline void
xv6fs_store_le32(void *storage, uint value)
{
  uchar *p = storage;

  p[0] = value;
  p[1] = value >> 8;
  p[2] = value >> 16;
  p[3] = value >> 24;
}

static inline void
xv6fs_super_decode(struct superblock *sb, const void *storage)
{
  const uchar *p = storage;

  sb->magic = xv6fs_load_le32(p + 0);
  sb->size = xv6fs_load_le32(p + 4);
  sb->nblocks = xv6fs_load_le32(p + 8);
  sb->ninodes = xv6fs_load_le32(p + 12);
  sb->nlog = xv6fs_load_le32(p + 16);
  sb->logstart = xv6fs_load_le32(p + 20);
  sb->inodestart = xv6fs_load_le32(p + 24);
  sb->bmapstart = xv6fs_load_le32(p + 28);
}

static inline void
xv6fs_dinode_decode(struct dinode *inode, const void *storage)
{
  const uchar *p = storage;

  inode->type = (short)xv6fs_load_le16(p + 0);
  inode->major = (short)xv6fs_load_le16(p + 2);
  inode->minor = (short)xv6fs_load_le16(p + 4);
  inode->nlink = (short)xv6fs_load_le16(p + 6);
  inode->size = xv6fs_load_le32(p + 8);
  for (uint i = 0; i < NDIRECT + 1; i++)
    inode->addrs[i] = xv6fs_load_le32(p + 12 + i * sizeof(uint));
}

static inline void
xv6fs_dinode_encode(void *storage, const struct dinode *inode)
{
  uchar *p = storage;

  xv6fs_store_le16(p + 0, (ushort)inode->type);
  xv6fs_store_le16(p + 2, (ushort)inode->major);
  xv6fs_store_le16(p + 4, (ushort)inode->minor);
  xv6fs_store_le16(p + 6, (ushort)inode->nlink);
  xv6fs_store_le32(p + 8, inode->size);
  for (uint i = 0; i < NDIRECT + 1; i++)
    xv6fs_store_le32(p + 12 + i * sizeof(uint), inode->addrs[i]);
}

static inline void
xv6fs_dirent_decode(struct dirent *entry, const void *storage)
{
  const uchar *p = storage;

  entry->inum = xv6fs_load_le16(p);
  for (uint i = 0; i < DIRSIZ; i++)
    entry->name[i] = p[sizeof(ushort) + i];
}

static inline void
xv6fs_dirent_encode(void *storage, const struct dirent *entry)
{
  uchar *p = storage;

  xv6fs_store_le16(p, entry->inum);
  for (uint i = 0; i < DIRSIZ; i++)
    p[sizeof(ushort) + i] = entry->name[i];
}

_Static_assert(sizeof(struct superblock) == XV6FS_SUPER_BYTES,
               "xv6 superblock layout mismatch");
_Static_assert(sizeof(struct dinode) == XV6FS_DINODE_BYTES,
               "xv6 dinode layout mismatch");
_Static_assert(sizeof(struct dirent) == XV6FS_DIRENT_BYTES,
               "xv6 dirent layout mismatch");
_Static_assert(__builtin_offsetof(struct dinode, size) == 8 &&
                 __builtin_offsetof(struct dinode, addrs) == 12 &&
                 __builtin_offsetof(struct dirent, name) == 2,
               "xv6 disk field offset mismatch");

#endif // XV6_FSDISK_H
