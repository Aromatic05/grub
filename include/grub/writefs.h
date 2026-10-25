/* Shared command interface for writable filesystems. GPL-3.0-or-later. */
#ifndef GRUB_WRITEFS_H
#define GRUB_WRITEFS_H
#include <grub/disk.h>
#include <grub/dl.h>
#include <grub/datetime.h>
#define GRUB_WRITEFS_DRIVES 10
#define GRUB_WRITEFS_READ 1
#define GRUB_WRITEFS_WRITE 2
#define GRUB_WRITEFS_CREATE 4
struct grub_writefs_mount;
struct grub_writefs_file
{
  struct grub_writefs_mount *mount;
  void *data;
  grub_uint64_t offset, size, id;
};
struct grub_writefs_driver
{
  const char *name;
  grub_dl_t mod;
  struct grub_writefs_driver *next;
  /* Return nonzero when this driver handles DISK.  Probe must not write. */
  int (*probe) (grub_disk_t, const grub_uint8_t *);
  grub_err_t (*mount) (struct grub_writefs_mount *);
  grub_err_t (*umount) (struct grub_writefs_mount *);
  grub_err_t (*open) (struct grub_writefs_file *, const char *, unsigned);
  grub_ssize_t (*read) (struct grub_writefs_file *, void *, grub_size_t);
  grub_ssize_t (*write) (struct grub_writefs_file *, const void *, grub_size_t);
  grub_err_t (*truncate) (struct grub_writefs_file *);
  grub_err_t (*close) (struct grub_writefs_file *);
  grub_err_t (*mkdir) (struct grub_writefs_mount *, const char *);
  grub_err_t (*unlink) (struct grub_writefs_mount *, const char *);
  grub_err_t (*rename) (struct grub_writefs_mount *, const char *, const char *);
  grub_err_t (*touch) (struct grub_writefs_mount *, const char *,
                       const struct grub_datetime *, unsigned);
};

/* Resolve optional touch fields using the datetime module in the driver. */
static inline grub_err_t
grub_writefs_touch_datetime (const struct grub_datetime *values, unsigned count,
                             struct grub_datetime *dt)
{
  grub_int64_t timestamp;
  if (count == 6)
    *dt = *values;
  else if (grub_get_datetime (dt))
    return grub_errno;
  if (count > 0) dt->year = values->year;
  if (count > 1) dt->month = values->month;
  if (count > 2) dt->day = values->day;
  if (count > 3) dt->hour = values->hour;
  if (count > 4) dt->minute = values->minute;
  if (count > 5) dt->second = values->second;
  if (dt->hour > 23 || dt->minute > 59 || dt->second > 59 ||
      !grub_datetime2unixtime (dt, &timestamp))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid date");
  return GRUB_ERR_NONE;
}
struct grub_writefs_mount
{
  grub_disk_t disk;
  struct grub_writefs_driver *driver;
  void *data;
  unsigned num, open_files;
};
#ifndef GRUB_LST_GENERATOR
void EXPORT_FUNC(grub_writefs_register) (struct grub_writefs_driver *);
#endif
void EXPORT_FUNC(grub_writefs_unregister) (struct grub_writefs_driver *);
grub_err_t EXPORT_FUNC(grub_writefs_mount) (const char *, unsigned);
grub_err_t EXPORT_FUNC(grub_writefs_umount) (unsigned);
struct grub_writefs_mount *EXPORT_FUNC(grub_writefs_get) (unsigned);
void grub_writefs_init (void);
#endif
