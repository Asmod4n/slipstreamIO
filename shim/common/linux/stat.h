/* struct statx and its masks, for hosts whose libc has none - the
 * shape is the kernel's, and the engine's off-Linux statx fallback
 * fills it from fstatat. Only the basic-stats fields are ever set
 * there, and stx_mask says exactly that. */
#ifndef SLIPSTREAM_SHIM_LINUX_STAT_H
#define SLIPSTREAM_SHIM_LINUX_STAT_H

#include <linux/types.h>

struct statx_timestamp {
  __s64 tv_sec;
  __u32 tv_nsec;
  __s32 __reserved;
};

struct statx {
  __u32 stx_mask;
  __u32 stx_blksize;
  __u64 stx_attributes;
  __u32 stx_nlink;
  __u32 stx_uid;
  __u32 stx_gid;
  __u16 stx_mode;
  __u16 __spare0[1];
  __u64 stx_ino;
  __u64 stx_size;
  __u64 stx_blocks;
  __u64 stx_attributes_mask;
  struct statx_timestamp stx_atime;
  struct statx_timestamp stx_btime;
  struct statx_timestamp stx_ctime;
  struct statx_timestamp stx_mtime;
  __u32 stx_rdev_major;
  __u32 stx_rdev_minor;
  __u32 stx_dev_major;
  __u32 stx_dev_minor;
  __u64 stx_mnt_id;
  __u32 stx_dio_mem_align;
  __u32 stx_dio_offset_align;
  __u64 __spare3[12];
};

#define STATX_TYPE 0x0001U
#define STATX_MODE 0x0002U
#define STATX_NLINK 0x0004U
#define STATX_UID 0x0008U
#define STATX_GID 0x0010U
#define STATX_ATIME 0x0020U
#define STATX_MTIME 0x0040U
#define STATX_CTIME 0x0080U
#define STATX_INO 0x0100U
#define STATX_SIZE 0x0200U
#define STATX_BLOCKS 0x0400U
#define STATX_BASIC_STATS 0x07ffU
#define STATX_BTIME 0x0800U

#endif
