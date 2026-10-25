#ifndef GRUB_NTFS_TIME_H
#define GRUB_NTFS_TIME_H
#include <grub/types.h>
typedef grub_int64_t time_t;
typedef int clockid_t;
#define __timespec_defined 1
struct timespec { time_t tv_sec; long tv_nsec; };
#define CLOCK_REALTIME 0
time_t time (time_t *);
int clock_gettime (clockid_t, struct timespec *);
#endif
