/* GRUB runtime and logging for ntfs-3g. GPL-3.0-or-later. */
#include "grub_ntfs_config.h"
#include <grub/datetime.h>
#include "logging.h"
int grub_ntfs_errno;
char *grub_ntfs_strcat (char *dst, const char *src)
{
  grub_strcpy (dst + grub_strlen (dst), src);
  return dst;
}
char *grub_ntfs_strerror (int err)
{
  static char message[32];
  grub_snprintf (message, sizeof (message), "NTFS error %d", err);
  return message;
}
int clock_gettime (clockid_t id, struct timespec *now)
{
  struct grub_datetime dt;
  now->tv_sec = 0;
  now->tv_nsec = 0;
  if (id != CLOCK_REALTIME || grub_get_datetime (&dt) ||
      !grub_datetime2unixtime (&dt, &now->tv_sec))
    { errno = EIO; return -1; }
  return 0;
}
int ntfs_log_redirect (const char *function __attribute__((unused)),
                       const char *file __attribute__((unused)),
                       int line __attribute__((unused)), u32 level,
                       void *data __attribute__((unused)), const char *format, ...)
{
  va_list args;
  char message[512];
  int saved = errno;
  if (!(level & (NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR | NTFS_LOG_LEVEL_CRITICAL)))
    return 0;
  va_start (args, format);
  grub_vsnprintf (message, sizeof (message), format, args);
  va_end (args);
  grub_printf ("ntfs3g: %s", message);
  errno = saved;
  return 0;
}

time_t time (time_t *result)
{
  struct timespec now;
  if (clock_gettime (CLOCK_REALTIME, &now)) return -1;
  if (result) *result = now.tv_sec;
  return now.tv_sec;
}
void ntfs_log_early_error (const char *format, ...)
{
  char message[512];
  va_list args;
  va_start (args, format);
  grub_vsnprintf (message, sizeof (message), format, args);
  va_end (args);
  grub_printf ("ntfs3g: %s", message);
}

/* i386 GRUB does not export the compiler's signed 64-bit division helper. */
#ifdef __i386__
grub_int64_t __divdi3 (grub_int64_t numerator, grub_int64_t denominator);
grub_int64_t __divdi3 (grub_int64_t numerator, grub_int64_t denominator)
{
  grub_uint64_t a = numerator < 0 ? -(grub_uint64_t) numerator : (grub_uint64_t) numerator;
  grub_uint64_t b = denominator < 0 ? -(grub_uint64_t) denominator : (grub_uint64_t) denominator;
  grub_uint64_t quotient = grub_divmod64 (a, b, NULL);
  return (numerator < 0) != (denominator < 0) ? -quotient : quotient;
}
#endif
