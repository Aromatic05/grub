/* POSIX error numbers, kept separate from grub_errno. */
#ifndef GRUB_NTFS_ERRNO_H
#define GRUB_NTFS_ERRNO_H
extern int grub_ntfs_errno;
#undef errno
#define errno grub_ntfs_errno
#define EACCES 13
#define EAGAIN 11
#define EBUSY 16
#define EEXIST 17
#define EFAULT 14
#define EILSEQ 84
#define EINTR 4
#define EINVAL 22
#define EIO 5
#define ELIBACC 79
#define ELIBBAD 80
#define EMLINK 31
#define EMSGSIZE 90
#define ENAMETOOLONG 36
#define ENODATA 61
#define ENOENT 2
#define ENOEXEC 8
#define ENOMEM 12
#define ENOSPC 28
#define ENOSYS 38
#define ENOTDIR 20
#define ENOTEMPTY 39
#define ENOTSUP 95
#define ENOTTY 25
#define ENXIO 6
#define EOPNOTSUPP 95
#define EOVERFLOW 75
#define EPERM 1
#define ERANGE 34
#define EROFS 30
#define ESPIPE 29
#define EISDIR 21
#endif
