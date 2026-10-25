/* ntfs-3g device operations backed by GRUB disks. GPL-3.0-or-later. */
#include "grub_ntfs_config.h"
#include <grub/disk.h>
#include "device.h"
struct ntfs_grub_device { grub_disk_t disk; s64 offset; int failed; };
static int device_open (struct ntfs_device *dev, int flags)
{
  if (NDevOpen (dev)) { errno = EBUSY; return -1; }
  NDevSetOpen (dev);
  if ((flags & 3) == O_RDONLY) NDevSetReadOnly (dev);
  return 0;
}
static int device_sync (struct ntfs_device *dev)
{
  struct ntfs_grub_device *io = dev->d_private;
  /* grub_disk_write is synchronous; this adapter has no pending write queue. */
  if (io->failed) { errno = EIO; return -1; }
  NDevClearDirty (dev);
  return 0;
}
static int device_close (struct ntfs_device *dev)
{
  int res = device_sync (dev);
  NDevClearOpen (dev);
  return res;
}
static s64 device_seek (struct ntfs_device *dev, s64 offset, int whence)
{
  struct ntfs_grub_device *io = dev->d_private;
  s64 base, size = (s64) (grub_disk_native_sectors (io->disk) << 9);
  if (whence == SEEK_SET) base = 0;
  else if (whence == SEEK_CUR) base = io->offset;
  else if (whence == SEEK_END) base = size;
  else { errno = EINVAL; return -1; }
  if (offset < -base || offset > size - base) { errno = EINVAL; return -1; }
  return io->offset = base + offset;
}
static s64 device_transfer (struct ntfs_device *dev, void *buf, s64 count, s64 offset, int writing)
{
  struct ntfs_grub_device *io = dev->d_private;
  grub_uint64_t size = grub_disk_native_sectors (io->disk) << 9;
  grub_err_t err;
  if (offset < 0 || count < 0 || (grub_uint64_t) offset > size ||
      (grub_uint64_t) count > size - offset || (grub_uint64_t) count > GRUB_SSIZE_MAX)
    { errno = EINVAL; return -1; }
  if (writing && io->failed) { errno = EIO; return -1; }
  if (writing && NDevReadOnly (dev)) { errno = EROFS; return -1; }
  if (!count) return 0;
  /* POSIX errors are translated by the backend. A previous command error
     must not turn successful cleanup I/O into another device failure. */
  grub_error_push ();
  err = writing ? grub_disk_write (io->disk, offset >> 9, offset & 511, count, buf) :
                  grub_disk_read (io->disk, offset >> 9, offset & 511, count, buf);
  grub_error_pop ();
  if (err)
    { errno = EIO; if (writing) io->failed = 1; return -1; }
  if (writing) NDevSetDirty (dev);
  return count;
}
static s64 device_pread (struct ntfs_device *dev, void *buf, s64 count, s64 offset)
{ return device_transfer (dev, buf, count, offset, 0); }
static s64 device_pwrite (struct ntfs_device *dev, const void *buf, s64 count, s64 offset)
{ return device_transfer (dev, (void *) buf, count, offset, 1); }
static s64 device_read (struct ntfs_device *dev, void *buf, s64 count)
{
  struct ntfs_grub_device *io = dev->d_private;
  s64 n = device_pread (dev, buf, count, io->offset);
  if (n > 0) io->offset += n;
  return n;
}
static s64 device_write (struct ntfs_device *dev, const void *buf, s64 count)
{
  struct ntfs_grub_device *io = dev->d_private;
  s64 n = device_pwrite (dev, buf, count, io->offset);
  if (n > 0) io->offset += n;
  return n;
}
static int device_stat (struct ntfs_device *dev, struct stat *st)
{
  struct ntfs_grub_device *io = dev->d_private;
  memset (st, 0, sizeof (*st));
  st->st_mode = S_IFBLK;
  st->st_size = grub_disk_native_sectors (io->disk) << 9;
  return 0;
}
static int device_ioctl (struct ntfs_device *dev, unsigned long request, void *arg)
{
  struct ntfs_grub_device *io = dev->d_private;
  if (request == BLKGETSIZE64)
    { *(u64 *) arg = grub_disk_native_sectors (io->disk) << 9; return 0; }
  if (request == BLKSSZGET)
    { *(int *) arg = 1U << io->disk->log_sector_size; return 0; }
  errno = EOPNOTSUPP;
  return -1;
}
struct ntfs_device_operations ntfs_grub_io_ops = {
  .open = device_open, .close = device_close, .seek = device_seek,
  .read = device_read, .write = device_write, .pread = device_pread,
  .pwrite = device_pwrite, .sync = device_sync, .stat = device_stat,
  .ioctl = device_ioctl
};
