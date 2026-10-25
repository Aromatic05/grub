/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/writefs.h>
#include <grub/err.h>
#include <grub/misc.h>
#include "ff.h"
#include "diskio.h"

GRUB_MOD_LICENSE ("GPLv3+");
static FATFS fatfs_list[GRUB_WRITEFS_DRIVES];
static struct grub_writefs_driver fat_driver;

static int
fat_backend_probe (grub_disk_t disk __attribute__ ((unused)),
                   const grub_uint8_t *boot)
{
  if (grub_memcmp (boot + 3, "EXFAT   ", 8) == 0)
    return 1;
  /* FAT boot sector BPB: exclude other jump-based formats such as NTFS. */
  return (boot[0] == 0xeb || boot[0] == 0xe9 || boot[0] == 0xe8)
    && boot[510] == 0x55 && boot[511] == 0xaa
    && boot[11] == 0 && (boot[12] == 2 || boot[12] == 4 ||
                          boot[12] == 8 || boot[12] == 16)
    && boot[13] && !(boot[13] & (boot[13] - 1))
    && (boot[14] || boot[15]);
}

static grub_err_t fat_error (FRESULT res)
{
  if (!res) return GRUB_ERR_NONE;
  return grub_error (res == FR_NO_FILE || res == FR_NO_PATH ? GRUB_ERR_FILE_NOT_FOUND :
                     res == FR_NO_FILESYSTEM ? GRUB_ERR_BAD_FS :
                     res == FR_NOT_ENOUGH_CORE ? GRUB_ERR_OUT_OF_MEMORY : GRUB_ERR_WRITE_ERROR,
                     "FatFs error %u", (unsigned) res);
}
static grub_err_t fat_backend_mount (struct grub_writefs_mount *mount)
{
  char dev[3];
  FRESULT res;
  unsigned n = mount->num;
  grub_snprintf (dev, sizeof (dev), "%u:", n);
  fat_stat[n].present = 1;
  fat_stat[n].disk = mount->disk;
  fat_stat[n].total_sectors = grub_disk_native_sectors (mount->disk);
  grub_snprintf (fat_stat[n].name, sizeof (fat_stat[n].name), "%u", n);
  res = f_mount (&fatfs_list[n], dev, 1);
  if (res)
    {
      f_mount (NULL, dev, 0);
      grub_memset (&fat_stat[n], 0, sizeof (fat_stat[n]));
      return fat_error (res);
    }
  mount->data = &fatfs_list[n];
  return GRUB_ERR_NONE;
}
static grub_err_t fat_backend_umount (struct grub_writefs_mount *mount)
{
  char dev[3];
  FRESULT res;
  grub_snprintf (dev, sizeof (dev), "%u:", mount->num);
  res = f_mount (NULL, dev, 0);
  grub_memset (&fat_stat[mount->num], 0, sizeof (fat_stat[mount->num]));
  grub_memset (&fatfs_list[mount->num], 0, sizeof (FATFS));
  return fat_error (res);
}
static grub_err_t fat_backend_open (struct grub_writefs_file *file, const char *path, unsigned flags)
{
  FIL *fp = grub_zalloc (sizeof (*fp));
  FRESULT res;
  BYTE mode = ((flags & GRUB_WRITEFS_READ) ? FA_READ : 0) |
              ((flags & GRUB_WRITEFS_WRITE) ? FA_WRITE : 0);
  if (!fp) return grub_errno;
  if (flags & GRUB_WRITEFS_CREATE) mode |= FA_OPEN_ALWAYS;
  res = f_open (fp, path, mode);
  if (res) { grub_free (fp); return fat_error (res); }
  file->data = fp;
  file->size = f_size (fp);
  if (fp->obj.fs->fs_type == FS_EXFAT)
    file->id = ((grub_uint64_t) fp->obj.c_scl << 32) | fp->obj.c_ofs;
  else
    file->id = (fp->dir_sect << 9) + (fp->dir_ptr - fp->obj.fs->win);
  return GRUB_ERR_NONE;
}
static grub_ssize_t fat_backend_read (struct grub_writefs_file *file, void *buf, grub_size_t len)
{
  FIL *fp = file->data;
  UINT n;
  FRESULT res = f_lseek (fp, file->offset);
  if (!res) res = f_read (fp, buf, len, &n);
  if (res) { fat_error (res); return -1; }
  file->offset += n;
  return n;
}
static grub_ssize_t fat_backend_write (struct grub_writefs_file *file, const void *buf, grub_size_t len)
{
  FIL *fp = file->data;
  UINT n;
  FRESULT res;
  /* FatFs leaves an expanded seek gap undefined. Make it deterministic. */
  if (file->offset > f_size (fp))
    {
      grub_uint8_t zero[512] = { 0 };
      res = f_lseek (fp, f_size (fp));
      if (res) { fat_error (res); return -1; }
      while (f_tell (fp) < file->offset)
        {
          UINT chunk = file->offset - f_tell (fp) > sizeof (zero) ? sizeof (zero) : file->offset - f_tell (fp);
          res = f_write (fp, zero, chunk, &n);
          if (res || n != chunk)
            { if (res) fat_error (res); else grub_error (GRUB_ERR_WRITE_ERROR, "FatFs disk full"); return -1; }
        }
    }
  res = f_lseek (fp, file->offset);
  if (res || f_tell (fp) != file->offset)
    { if (res) fat_error (res); else grub_error (GRUB_ERR_OUT_OF_RANGE, "FatFs offset out of range"); return -1; }
  res = f_write (fp, buf, len, &n);
  if (res) { fat_error (res); return -1; }
  file->offset += n;
  file->size = f_size (fp);
  if (n != len) grub_error (GRUB_ERR_WRITE_ERROR, "FatFs disk full");
  return n;
}
static grub_err_t fat_backend_truncate (struct grub_writefs_file *file)
{
  FIL *fp = file->data;
  FRESULT res = f_lseek (fp, file->offset);
  if (!res) res = f_truncate (fp);
  file->size = f_size (fp);
  return fat_error (res);
}
static grub_err_t fat_backend_close (struct grub_writefs_file *file)
{
  FRESULT res = f_close (file->data);
  grub_free (file->data);
  return fat_error (res);
}
static grub_err_t fat_backend_mkdir (struct grub_writefs_mount *mount __attribute__((unused)), const char *path)
{ return fat_error (f_mkdir (path)); }
static grub_err_t fat_backend_unlink (struct grub_writefs_mount *mount __attribute__((unused)), const char *path)
{ return fat_error (f_unlink (path)); }
static grub_err_t fat_backend_rename (struct grub_writefs_mount *mount __attribute__((unused)), const char *a, const char *b)
{ return fat_error (f_rename (a, b)); }
static grub_err_t fat_backend_touch (struct grub_writefs_mount *mount __attribute__((unused)), const char *path, const struct grub_datetime *values, unsigned count)
{
  struct grub_datetime resolved;
  const struct grub_datetime *dt = &resolved;
  FILINFO info;
  FIL file;
  FRESULT res;
  if (grub_writefs_touch_datetime (values, count, &resolved))
    return grub_errno;
  if (dt->year < 1980 || dt->year > 2107)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "FAT timestamps require year 1980-2107");
  res = f_stat (path, &info);
  if (res == FR_NO_FILE)
    {
      res = f_open (&file, path, FA_WRITE | FA_CREATE_NEW);
      if (!res) res = f_close (&file);
    }
  if (res) return fat_error (res);
  info.fdate = ((dt->year - 1980) << 9) | (dt->month << 5) | dt->day;
  info.ftime = (dt->hour << 11) | (dt->minute << 5) | (dt->second >> 1);
  return fat_error (f_utime (path, &info));
}
static grub_err_t
fat_backend_get_label (struct grub_writefs_mount *mount, char **out)
{
  char device[3], label[64];
  FRESULT res;
  grub_snprintf (device, sizeof (device), "%u:", mount->num);
  res = f_getlabel (device, label, NULL);
  if (res) return fat_error (res);
  *out = grub_strdup (label);
  return *out ? GRUB_ERR_NONE : grub_errno;
}

static grub_err_t
fat_backend_set_label (struct grub_writefs_mount *mount, const char *label)
{
  char *with_drive = grub_xasprintf ("%u:%s", mount->num, label);
  FRESULT res;
  if (!with_drive) return grub_errno;
  res = f_setlabel (with_drive);
  grub_free (with_drive);
  return fat_error (res);
}

static struct grub_writefs_driver fat_driver = {
  .name = "fatfs", .probe = fat_backend_probe,
  .mount = fat_backend_mount, .umount = fat_backend_umount,
  .open = fat_backend_open, .read = fat_backend_read, .write = fat_backend_write,
  .truncate = fat_backend_truncate, .close = fat_backend_close,
  .mkdir = fat_backend_mkdir, .unlink = fat_backend_unlink,
  .rename = fat_backend_rename, .touch = fat_backend_touch,
  .get_label = fat_backend_get_label, .set_label = fat_backend_set_label
};

GRUB_MOD_INIT(fatfs)
{
  fat_driver.mod = mod;
  grub_writefs_register (&fat_driver, "fat12,fat16,fat32,exfat");
}
GRUB_MOD_FINI(fatfs)
{
  grub_writefs_unregister (&fat_driver);
}
