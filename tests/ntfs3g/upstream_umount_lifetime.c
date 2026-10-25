/* Reproducer for ntfs_umount's documented error lifetime, using unmodified upstream. */
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/device.h>

static int fail_sync (struct ntfs_device *dev)
{
  (void) dev;
  errno = EIO;
  return -1;
}
int main (int argc, char **argv)
{
  ntfs_volume *vol;
  struct ntfs_device_operations ops;
  struct ntfs_device *volatile still_owned;
  int result;
  assert (argc == 2);
  vol = ntfs_mount (argv[1], NTFS_MNT_EXCLUSIVE);
  assert (vol != NULL);
  ops = *vol->dev->d_ops;
  ops.sync = fail_sync;
  vol->dev->d_ops = &ops;
  result = ntfs_umount (vol, FALSE);
  assert (result == -1);
  assert (errno == EIO);
  /* The API comment promises the volume can still be used after an error. */
  still_owned = vol->dev;
  assert (still_owned != NULL);
  puts ("volume remains valid after failed unmount");
  return 0;
}
