#ifndef GRUB_NTFS_SYS_TYPES_H
#define GRUB_NTFS_SYS_TYPES_H
#include_next <sys/types.h>
typedef grub_int32_t pid_t;
typedef grub_uint32_t uid_t;
typedef grub_uint32_t gid_t;
typedef grub_uint32_t dev_t;
typedef grub_uint32_t mode_t;
typedef grub_int64_t off_t;
#endif
