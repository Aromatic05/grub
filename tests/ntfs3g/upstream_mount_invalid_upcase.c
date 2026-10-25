/* Run against an image with a one-byte $UpCase data attribute. */
#include <assert.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <ntfs-3g/volume.h>
int main (int argc, char **argv)
{
  assert (argc == 2);
  assert (ntfs_mount (argv[1], NTFS_MNT_RDONLY) == NULL);
  return 0;
}
