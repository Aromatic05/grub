/* cpio.c - cpio and tar filesystem.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2007,2008,2009,2013 Free Software Foundation, Inc.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/disk.h>
#include <grub/extcmd.h>
#include <grub/file.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/safemath.h>

#define ALIGN_CPIO(x) (ALIGN_UP ((x), 4))
#define	MAGIC	"070701"
#define	MAGIC2	"070702"
struct head
{
  char magic[6];
  char ino[8];
  char mode[8];
  char uid[8];
  char gid[8];
  char nlink[8];
  char mtime[8];
  char filesize[8];
  char devmajor[8];
  char devminor[8];
  char rdevmajor[8];
  char rdevminor[8];
  char namesize[8];
  char check[8];
} GRUB_PACKED;

static inline unsigned long long
read_number (const char *str, grub_size_t size)
{
  unsigned long long ret = 0;
  while (size-- && grub_isxdigit (*str))
    {
      char dig = *str++;
      if (dig >= '0' && dig <= '9')
	dig &= 0xf;
      else if (dig >= 'a' && dig <= 'f')
	dig -= 'a' - 10;
      else
	dig -= 'A' - 10;
      ret = (ret << 4) | (dig);
    }
  return ret;
}

#define FSNAME "newc"

#include "cpio_common.c"

/* mkinitrd: editable newc archives kept in memory and exposed as disks.  */

#define TRAILER "TRAILER!!!"
#define MKINITRD_GROW (1 << 20)

struct grub_initrd
{
  char *name;
  grub_uint8_t *data;
  grub_size_t size;
  grub_size_t capacity;
  unsigned long id;
  struct grub_initrd *next;
};

static struct grub_initrd *initrd_list;
static unsigned long initrd_last_id;

static struct grub_initrd *
initrd_find (const char *name)
{
  struct grub_initrd *dev;

  for (dev = initrd_list; dev; dev = dev->next)
    if (grub_strcmp (dev->name, name) == 0)
      return dev;
  return NULL;
}

static struct grub_initrd *
initrd_get (const char *name)
{
  struct grub_initrd *dev = initrd_find (name);

  if (!dev)
    grub_error (GRUB_ERR_UNKNOWN_DEVICE, N_("disk `%s' not found"), name);
  return dev;
}

static int
grub_initrd_iterate (grub_disk_dev_iterate_hook_t hook, void *hook_data,
		     grub_disk_pull_t pull)
{
  struct grub_initrd *dev;

  if (pull != GRUB_DISK_PULL_NONE)
    return 0;
  for (dev = initrd_list; dev; dev = dev->next)
    if (hook (dev->name, hook_data))
      return 1;
  return 0;
}

static grub_err_t
grub_initrd_open (const char *name, grub_disk_t disk)
{
  struct grub_initrd *dev = initrd_find (name);

  if (!dev)
    return grub_error (GRUB_ERR_UNKNOWN_DEVICE, "can't open device");
  disk->total_sectors = ALIGN_UP (dev->size, GRUB_DISK_SECTOR_SIZE)
			>> GRUB_DISK_SECTOR_BITS;
  disk->max_agglomerate = GRUB_DISK_MAX_MAX_AGGLOMERATE;
  disk->id = dev->id;
  disk->data = dev;
  return GRUB_ERR_NONE;
}

/*
 * Byte range of a sector request inside the archive.  The disk is rounded
 * up to whole sectors, so the tail beyond the archive end reads as zeros.
 */
static grub_err_t
initrd_range (struct grub_initrd *dev, grub_disk_addr_t sector,
	      grub_size_t count, grub_size_t range[3])
{
  grub_size_t offset = 0, length = 0;

  if (grub_mul (sector, (grub_disk_addr_t) GRUB_DISK_SECTOR_SIZE, &offset)
      || grub_mul (count, (grub_size_t) GRUB_DISK_SECTOR_SIZE, &length)
      || offset > dev->size)
    return grub_error (GRUB_ERR_OUT_OF_RANGE,
		       N_("attempt to read or write outside of disk `%s'"),
		       dev->name);
  range[0] = offset;
  range[1] = length;
  range[2] = dev->size - offset < length ? dev->size - offset : length;
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_initrd_read (grub_disk_t disk, grub_disk_addr_t sector,
		  grub_size_t count, char *buf)
{
  struct grub_initrd *dev = disk->data;
  grub_size_t range[3] = { 0, 0, 0 };

  if (initrd_range (dev, sector, count, range))
    return grub_errno;
  grub_memcpy (buf, dev->data + range[0], range[2]);
  grub_memset (buf + range[2], 0, range[1] - range[2]);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_initrd_write (grub_disk_t disk, grub_disk_addr_t sector,
		   grub_size_t count, const char *buf)
{
  struct grub_initrd *dev = disk->data;
  grub_size_t range[3] = { 0, 0, 0 };

  if (initrd_range (dev, sector, count, range))
    return grub_errno;
  grub_memcpy (dev->data + range[0], buf, range[2]);
  return GRUB_ERR_NONE;
}

static struct grub_disk_dev grub_initrd_dev =
  {
    .name = "initrd",
    .id = GRUB_DISK_DEVICE_INITRD_ID,
    .disk_iterate = grub_initrd_iterate,
    .disk_open = grub_initrd_open,
    .disk_read = grub_initrd_read,
    .disk_write = grub_initrd_write,
    .next = 0
  };

struct initrd_entry
{
  grub_size_t offset;
  grub_size_t name_offset;
  grub_size_t name_size;
  grub_size_t next;
};

/* Parse the newc header at OFFSET.  */
static grub_err_t
initrd_entry (struct grub_initrd *dev, grub_size_t offset,
	      struct initrd_entry *entry)
{
  struct head hd;
  grub_size_t name_end, data_end;
  grub_size_t name_size, file_size;

  if (dev->size - offset < sizeof (hd))
    return grub_error (GRUB_ERR_BAD_FS, "truncated newc header in `%s'", dev->name);
  grub_memcpy (&hd, dev->data + offset, sizeof (hd));
  if (grub_memcmp (hd.magic, MAGIC, sizeof (hd.magic)) != 0
      && grub_memcmp (hd.magic, MAGIC2, sizeof (hd.magic)) != 0)
    return grub_error (GRUB_ERR_BAD_FS, "not a newc archive: `%s'", dev->name);

  name_size = read_number (hd.namesize, ARRAY_SIZE (hd.namesize));
  file_size = read_number (hd.filesize, ARRAY_SIZE (hd.filesize));
  if (name_size == 0
      || grub_add (offset + sizeof (hd), name_size, &name_end)
      || name_end > dev->size
      || grub_add (ALIGN_CPIO (name_end), file_size, &data_end)
      || ALIGN_CPIO (data_end) < data_end)
    return grub_error (GRUB_ERR_BAD_FS, "corrupt newc entry in `%s'", dev->name);

  entry->offset = offset;
  entry->name_offset = offset + sizeof (hd);
  entry->name_size = name_size;
  entry->next = ALIGN_CPIO (data_end);
  return GRUB_ERR_NONE;
}

static int
initrd_entry_is (struct grub_initrd *dev, const struct initrd_entry *entry,
		 const char *name)
{
  grub_size_t len = grub_strlen (name);

  return entry->name_size == len + 1
    && grub_memcmp (dev->data + entry->name_offset, name, len + 1) == 0;
}

/*
 * Walk every entry of every concatenated archive.  Stops early when FIND
 * matches; otherwise *TRAILER receives the offset of the last trailer, or
 * the archive end when no trailer exists.
 */
static grub_err_t
initrd_walk (struct grub_initrd *dev, const char *find,
	     struct initrd_entry *found, grub_size_t *trailer)
{
  struct initrd_entry entry;
  grub_size_t offset = 0;

  if (trailer)
    *trailer = dev->size;
  while (offset < dev->size)
    {
      if (initrd_entry (dev, offset, &entry))
	return grub_errno;
      if (find && initrd_entry_is (dev, &entry, find))
	{
	  *found = entry;
	  return GRUB_ERR_NONE;
	}
      offset = entry.next;
      if (initrd_entry_is (dev, &entry, TRAILER))
	{
	  if (trailer)
	    *trailer = entry.offset;
	  /* Concatenated archives may be separated by zero padding.  */
	  while (offset < dev->size && dev->data[offset] == 0)
	    offset = ALIGN_CPIO (offset + 1);
	}
    }
  if (find)
    return grub_error (GRUB_ERR_FILE_NOT_FOUND, N_("file `%s' not found"), find);
  return GRUB_ERR_NONE;
}

static char
hex_digit (grub_uint8_t val)
{
  return val < 10 ? '0' + val : 'a' + val - 10;
}

static void
set_field (char *field, grub_uint32_t val)
{
  int shift;

  for (shift = 28; shift >= 0; shift -= 4)
    *field++ = hex_digit ((val >> shift) & 0xf);
}

static grub_size_t
header_size (const char *name)
{
  return ALIGN_CPIO (sizeof (struct head) + grub_strlen (name) + 1);
}

static grub_uint8_t *
make_header (grub_uint8_t *ptr, const char *name, grub_uint32_t mode,
	     grub_uint32_t file_size)
{
  static grub_uint32_t ino = 0xfffffff0;
  struct head *hd = (struct head *) ptr;
  grub_uint32_t name_size = grub_strlen (name) + 1;
  grub_size_t size = header_size (name);

  grub_memset (ptr, 0, size);
  grub_memcpy (hd->magic, MAGIC, sizeof (hd->magic));
  set_field (hd->ino, ino--);
  set_field (hd->mode, mode);
  set_field (hd->uid, 0);
  set_field (hd->gid, 0);
  set_field (hd->nlink, 1);
  set_field (hd->mtime, 0);
  set_field (hd->filesize, file_size);
  set_field (hd->devmajor, 0);
  set_field (hd->devminor, 0);
  set_field (hd->rdevmajor, 0);
  set_field (hd->rdevminor, 0);
  set_field (hd->namesize, name_size);
  set_field (hd->check, 0);
  grub_memcpy (ptr + sizeof (*hd), name, name_size);
  return ptr + size;
}

static grub_err_t
initrd_reserve (struct grub_initrd *dev, grub_size_t size)
{
  grub_size_t capacity;
  grub_uint8_t *data;

  if (size <= dev->capacity)
    return GRUB_ERR_NONE;
  if (grub_add (size, (grub_size_t) MKINITRD_GROW, &capacity))
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "initrd too large");
  data = grub_realloc (dev->data, capacity);
  if (!data)
    return grub_errno;
  dev->data = data;
  dev->capacity = capacity;
  return GRUB_ERR_NONE;
}

static grub_err_t
initrd_read_file (grub_file_t file, void *buf)
{
  if (grub_file_read (file, buf, file->size) != (grub_ssize_t) file->size)
    {
      if (!grub_errno)
	grub_error (GRUB_ERR_FILE_READ_ERROR, N_("premature end of file %s"),
		    file->name);
      return grub_errno;
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
initrd_create (const char *name, grub_file_t file)
{
  struct grub_initrd *dev;
  grub_size_t size = file ? file->size : 0;
  grub_uint8_t *data;

  if (file && file->size != size)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "initrd too large");
  data = grub_malloc (size + MKINITRD_GROW);
  if (!data)
    return grub_errno;
  if (file && initrd_read_file (file, data))
    {
      grub_free (data);
      return grub_errno;
    }

  dev = initrd_find (name);
  if (!dev)
    {
      dev = grub_zalloc (sizeof (*dev));
      if (dev)
	dev->name = grub_strdup (name);
      if (!dev || !dev->name)
	{
	  if (dev)
	    grub_free (dev);
	  grub_free (data);
	  return grub_errno;
	}
      dev->id = initrd_last_id++;
      dev->next = initrd_list;
      initrd_list = dev;
    }
  grub_free (dev->data);
  dev->data = data;
  dev->size = size;
  dev->capacity = size + MKINITRD_GROW;
  return GRUB_ERR_NONE;
}

static grub_err_t
initrd_add (const char *dev_name, grub_file_t file, const char *name)
{
  struct grub_initrd *dev = initrd_get (dev_name);
  grub_size_t offset, end, size;
  grub_uint8_t *ptr;

  if (!dev)
    return grub_errno;
  if (file->size > GRUB_UINT_MAX)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "file too large for newc");
  if (initrd_walk (dev, NULL, NULL, &offset))
    return grub_errno;

  if (grub_add (offset, header_size (name), &end)
      || grub_add (end, (grub_size_t) ALIGN_CPIO (file->size), &end)
      || grub_add (end, header_size (TRAILER), &end))
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "initrd too large");
  if (initrd_reserve (dev, end))
    return grub_errno;

  ptr = make_header (dev->data + offset, name, 0100777, file->size);
  if (initrd_read_file (file, ptr))
    return grub_errno;
  size = ALIGN_CPIO (file->size);
  grub_memset (ptr + file->size, 0, size - file->size);
  make_header (ptr + size, TRAILER, 0, 0);
  dev->size = end;
  return GRUB_ERR_NONE;
}

static grub_err_t
initrd_rename (const char *dev_name, const char *name, const char *new_name)
{
  struct grub_initrd *dev = initrd_get (dev_name);
  struct initrd_entry entry;

  if (!dev)
    return grub_errno;
  if (grub_strlen (name) != grub_strlen (new_name))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "namesize mismatch");
  if (initrd_walk (dev, name, &entry, NULL))
    return grub_errno;
  grub_memcpy (dev->data + entry.name_offset, new_name, grub_strlen (new_name));
  return GRUB_ERR_NONE;
}

static void
initrd_delete (const char *name)
{
  struct grub_initrd **prev, *dev;

  for (prev = &initrd_list; *prev; prev = &(*prev)->next)
    if (grub_strcmp ((*prev)->name, name) == 0)
      {
	dev = *prev;
	*prev = dev->next;
	grub_free (dev->name);
	grub_free (dev->data);
	grub_free (dev);
	return;
      }
}

static const struct grub_arg_option mkinitrd_options[] =
  {
    {"create", 'c', 0, N_("Create an initrd."), 0, 0},
    {"add", 'a', 0, N_("Copy a file to initrd."), 0, 0},
    {"ren", 'r', 0, N_("Rename file."), 0, 0},
    {"delete", 'd', 0, N_("Delete the specified initrd."), 0, 0},
    {0, 0, 0, 0, 0, 0}
  };

enum
  {
    MKINITRD_CREATE,
    MKINITRD_ADD,
    MKINITRD_REN,
    MKINITRD_DELETE,
  };

static grub_err_t
grub_cmd_mkinitrd (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  grub_file_t file = NULL;
  grub_err_t err;

  if (state[MKINITRD_CREATE].set)
    {
      if (argc < 1)
	return grub_error (GRUB_ERR_BAD_ARGUMENT, "Usage: --create DEV [FILE]");
      if (argc > 1)
	{
	  file = grub_file_open (args[1], GRUB_FILE_TYPE_LOOPBACK);
	  if (!file)
	    return grub_errno;
	}
      err = initrd_create (args[0], file);
    }
  else if (state[MKINITRD_ADD].set)
    {
      if (argc < 3)
	return grub_error (GRUB_ERR_BAD_ARGUMENT, "Usage: --add DEV FILE NAME");
      file = grub_file_open (args[1], GRUB_FILE_TYPE_LOOPBACK);
      if (!file)
	return grub_errno;
      err = initrd_add (args[0], file, args[2]);
    }
  else if (state[MKINITRD_REN].set)
    {
      if (argc < 3)
	return grub_error (GRUB_ERR_BAD_ARGUMENT,
			   "Usage: --ren DEV OLDNAME NEWNAME");
      err = initrd_rename (args[0], args[1], args[2]);
    }
  else if (state[MKINITRD_DELETE].set)
    {
      if (argc < 1)
	return grub_error (GRUB_ERR_BAD_ARGUMENT, "Usage: --delete DEV");
      initrd_delete (args[0]);
      err = GRUB_ERR_NONE;
    }
  else
    err = grub_error (GRUB_ERR_BAD_ARGUMENT, N_("no option specified"));

  if (file)
    grub_file_close (file);
  return err;
}

static grub_extcmd_t cmd_mkinitrd;

GRUB_MOD_INIT (newc)
{
  grub_cpio_fs.mod = mod;
  grub_fs_register (&grub_cpio_fs);
  cmd_mkinitrd = grub_register_extcmd ("mkinitrd", grub_cmd_mkinitrd, 0,
				       N_("OPTIONS"),
				       N_("Make a virtual drive from a file."),
				       mkinitrd_options);
  grub_disk_dev_register (&grub_initrd_dev);
}

GRUB_MOD_FINI (newc)
{
  grub_disk_dev_unregister (&grub_initrd_dev);
  grub_unregister_extcmd (cmd_mkinitrd);
  grub_fs_unregister (&grub_cpio_fs);
}
