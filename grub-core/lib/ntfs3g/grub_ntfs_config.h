/* GRUB runtime configuration for the vendored ntfs-3g library. */
#ifndef GRUB_NTFS_CONFIG_H
#define GRUB_NTFS_CONFIG_H
#include <grub/types.h>
#include <grub/misc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#define GRUB_NTFS 1
#define HAVE_CONFIG_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STDIO_H 1
#define HAVE_ERRNO_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_TIME_H 1
#define HAVE_CLOCK_GETTIME 1
#define DISABLE_PLUGINS 1
#define FORCE_UTF8 1
#define IGNORE_MTAB 1
#define NO_NTFS_DEVICE_DEFAULT_IO_OPS 1
#define HAVE_FFS 1
#define VA_LIST va_list
#define VA_START va_start
#define VA_END va_end
#define BLKGETSIZE64 0x80081272
#define BLKSSZGET 0x1268
#define strrchr grub_strrchr
#define strdup grub_strdup
#define strncasecmp grub_strncasecmp
#define strcat grub_ntfs_strcat
#define strerror grub_ntfs_strerror
#define getuid() 0
#define getgid() 0
#define getpid() 1
#define getgrgid(x) NULL
#define getpwuid(x) NULL
#define major(x) 0
#define minor(x) 0
#define atoi(x) 0
#define random() 0
#define srandom(x) ((void) (x))
#define makedev(major, minor) (((major) << 16) | (minor))
#define exit(x) grub_abort()
#define ffs(x) __builtin_ffs(x)
char *grub_ntfs_strcat (char *, const char *);
char *grub_ntfs_strerror (int);
struct group { gid_t gr_gid; char **gr_mem; };
struct passwd { char *pw_name; uid_t pw_uid; gid_t pw_gid; };
static inline int open (const char *p __attribute__((unused)), int f __attribute__((unused))) { errno = ENOSYS; return -1; }
static inline int close (int f __attribute__((unused))) { errno = ENOSYS; return -1; }
static inline int read (int f __attribute__((unused)), void *b __attribute__((unused)), size_t n __attribute__((unused))) { errno = ENOSYS; return -1; }
#endif
