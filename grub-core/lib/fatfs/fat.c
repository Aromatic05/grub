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
#include <grub/lua.h>
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
  BYTE mode = (flags & GRUB_WRITEFS_WRITE) ? FA_WRITE : FA_READ;
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
static struct grub_writefs_driver fat_driver = {
  .name = "fatfs", .probe = fat_backend_probe,
  .mount = fat_backend_mount, .umount = fat_backend_umount,
  .open = fat_backend_open, .read = fat_backend_read, .write = fat_backend_write,
  .truncate = fat_backend_truncate, .close = fat_backend_close,
  .mkdir = fat_backend_mkdir, .unlink = fat_backend_unlink,
  .rename = fat_backend_rename, .touch = fat_backend_touch
};

/* fat.mount hdx,y disknum */
static int
fat_mount (lua_State *state)
{
  const char *name = luaL_checkstring (state, 1);
  int num = luaL_checkinteger (state, 2);
  struct grub_writefs_mount *mount;
  if (grub_writefs_mount (name, num)) return luaL_error (state, "fat.mount failed");
  mount = grub_writefs_get (num);
  if (mount->driver != &fat_driver)
    { grub_writefs_umount (num); return luaL_error (state, "expected FAT/exFAT volume"); }
  return 0;
}
static int
fat_umount (lua_State *state)
{
  unsigned num = luaL_checkinteger (state, 1);
  struct grub_writefs_mount *mount = grub_writefs_get (num);
  if (!mount || mount->driver != &fat_driver)
    return luaL_error (state, "expected mounted FAT/exFAT volume");
  if (grub_writefs_umount (num)) return luaL_error (state, "fat.umount failed");
  return 0;
}

/* fat.disk_status disknum @ret: grub_disk_t */
static int
fat_disk_status (lua_State *state)
{
  int num = 0;
  num = luaL_checkinteger (state, 1);
  if (num > 9 || num <= 0)
    return 0;
  if (!fat_stat[num].disk)
    return 0;
  lua_pushlightuserdata (state, fat_stat[num].disk);
  return 1;
}

/* fat.get_label disknum @ret:label */
static int
fat_get_label (lua_State *state)
{
  char label[35];
  int num = 0;
  char dev[3] = "1:";
  num = luaL_checkinteger (state, 1);
  if (num > 9 || num <= 0)
    return 0;
  grub_snprintf (dev, 3, "%d:", num);
  f_getlabel(dev, label, 0);
  lua_pushstring (state, label);
  return 1;
}

/* fat.set_label disknum label */
static int
fat_set_label (lua_State *state)
{
  const char *label;
  int num = 0;
  char dev[40] = "1:";
  num = luaL_checkinteger (state, 1);
  if (num > 9 || num <= 0)
    return 0;
  label = luaL_checkstring (state, 2);
  if (grub_strlen (label) > 34)
    return 0;
  grub_snprintf (dev, 40, "%d:%s", num, label);
  f_setlabel(dev);
  return 0;
}

/* fat.mkdir path */
static int
fat_mkdir (lua_State *state)
{
  const char *path;
  path = luaL_checkstring (state, 1);
  f_mkdir (path);
  return 0;
}

/* fat.rename path1 path2 */
static int
fat_rename (lua_State *state)
{
  const char *path1, *path2;
  path1 = luaL_checkstring (state, 1);
  path2 = luaL_checkstring (state, 2);
  f_rename (path1, path2);
  return 0;
}

/* fat.unlink path */
static int
fat_unlink (lua_State *state)
{
  const char *path;
  path = luaL_checkstring (state, 1);
  f_unlink (path);
  return 0;
}

/* fat.open path mode @ret:FIL */
static int
fat_open (lua_State *state)
{
  FIL *file = NULL;
  const char *name;
  BYTE flag = 0;

  file = grub_malloc (sizeof (FIL));
  if (! file)
    return 0;
  name = luaL_checkstring (state, 1);
  flag = (lua_gettop (state) > 1) ? luaL_checkinteger (state, 2) : 0;
  if (name[0] < '1' || name[0] > '9' || name[1] != ':' || name[2] != '/' ||
      !fat_stat[name[0] - '0'].disk)
    { grub_free (file); return luaL_error (state, "expected mounted FAT/exFAT path"); }
  if (f_open (file, name, flag))
    { grub_free (file); return luaL_error (state, "fat.open failed"); }
  grub_writefs_get (name[0] - '0')->open_files++;
  lua_pushlightuserdata (state, file);
  return 1;
}

/* fat.close FIL */
static int
fat_close (lua_State *state)
{
  FIL *file = NULL;

  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  {
    unsigned num;
    FRESULT res;
    for (num = 1; num < GRUB_WRITEFS_DRIVES; num++)
      if (file->obj.fs == &fatfs_list[num]) break;
    res = f_close (file);
    if (num < GRUB_WRITEFS_DRIVES) grub_writefs_get (num)->open_files--;
    grub_free (file);
    if (res) return luaL_error (state, "fat.close failed");
  }
  return 0;
}

/* fat.read FIL size @ret:buf */
static int
fat_read (lua_State *state)
{
  FIL *file = NULL;
  luaL_Buffer b;
  int n;
  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  n = luaL_checkinteger (state, 2);
  luaL_buffinit (state, &b);
  while (n)
  {
    char *p;
    UINT nr;

    nr = (n > LUAL_BUFFERSIZE) ? LUAL_BUFFERSIZE : n;
    p = luaL_prepbuffer (&b);

    f_read (file, p, nr, &nr);
    if (nr == 0)
      break;

    luaL_addsize (&b, nr);
    n -= nr;
  }
  luaL_pushresult (&b);
  return 1;
}

/* fat.write FIL buf @ret:size */
static int
fat_write (lua_State *state)
{
  FIL *file;
  UINT ret;
  grub_size_t len;
  const char *buf;

  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  buf = lua_tolstring (state, 2, &len);
  f_write (file, buf, len, &ret);

  lua_pushinteger (state, ret);
  return 1;
}

/* fat.lseek FIL ofs */
static int
fat_lseek (lua_State *state)
{
  FIL *file;
  int ofs;
  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  ofs = luaL_checkinteger (state, 2);
  f_lseek (file, ofs);
  return 0;
}

/* fat.tell FIL @ret:ofs */
static int
fat_tell (lua_State *state)
{
  FIL *file;
  int ret;
  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  ret = f_tell (file);
  lua_pushinteger (state, ret);
  return 1;
}

/* fat.eof FIL @ret:num */
static int
fat_eof (lua_State *state)
{
  FIL *file;
  int ret;
  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  ret = f_eof (file);
  lua_pushinteger (state, ret);
  return 1;
}

/* fat.size FIL @ret:size */
static int
fat_size (lua_State *state)
{
  FIL *file;
  int ret;
  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  ret = f_size (file);
  lua_pushinteger (state, ret);
  return 1;
}

/* fat.truncate FIL */
static int
fat_truncate (lua_State *state)
{
  FIL *file;
  luaL_checktype (state, 1, LUA_TLIGHTUSERDATA);
  file = lua_touserdata (state, 1);
  f_truncate (file);
  return 0;
}

static luaL_Reg fatlib[] =
{
  {"mount", fat_mount},
  {"umount", fat_umount},
  {"disk_status", fat_disk_status},
  {"get_label", fat_get_label},
  {"set_label", fat_set_label},
  {"mkdir", fat_mkdir},
  {"rename", fat_rename},
  {"unlink", fat_unlink},
  {"open", fat_open},
  {"close", fat_close},
  {"read", fat_read},
  {"write", fat_write},
  {"lseek", fat_lseek},
  {"tell", fat_tell},
  {"eof", fat_eof},
  {"size", fat_size},
  {"truncate", fat_truncate},
  {0, 0}
};


GRUB_MOD_INIT(fatfs)
{
  fat_driver.mod = mod;
  grub_writefs_register (&fat_driver);
  if (grub_lua_global_state)
    {
      lua_gc (grub_lua_global_state, LUA_GCSTOP, 0);
      luaL_register (grub_lua_global_state, "fat", fatlib);
      lua_gc (grub_lua_global_state, LUA_GCRESTART, 0);
    }
}
GRUB_MOD_FINI(fatfs)
{
  grub_writefs_unregister (&fat_driver);
  if (grub_lua_global_state)
    { lua_pushnil (grub_lua_global_state); lua_setglobal (grub_lua_global_state, "fat"); }
}
