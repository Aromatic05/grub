/* fs.c - filesystem manager */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2002,2005,2007  Free Software Foundation, Inc.
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

#include <grub/disk.h>
#include <grub/net.h>
#include <grub/fs.h>
#include <grub/file.h>
#include <grub/partition.h>
#include <grub/err.h>
#include <grub/misc.h>
#include <grub/types.h>
#include <grub/mm.h>
#include <grub/term.h>
#include <grub/i18n.h>

grub_fs_t grub_fs_list = 0;

grub_fs_autoload_hook_t grub_fs_autoload_hook = 0;

/* Helper for grub_fs_probe.  */
static int
probe_dummy_iter (const char *filename __attribute__ ((unused)),
		  const struct grub_dirhook_info *info __attribute__ ((unused)),
		  void *data __attribute__ ((unused)))
{
  return 1;
}

grub_fs_t
grub_fs_probe (grub_device_t device)
{
  grub_fs_t p;

  if (device->disk)
    {
      /* Make it sure not to have an infinite recursive calls.  */
      static int count = 0;

      for (p = grub_fs_list; p; p = p->next)
	{
	  grub_dprintf ("portdbg", "fs_probe: try fs=%s disk=%s\n", p->name,
			device->disk ? device->disk->name : "(null)");
	  grub_dprintf ("fs", "Detecting %s...\n", p->name);

	  /* This is evil: newly-created just mounted BtrFS after copying all
	     GRUB files has a very peculiar unrecoverable corruption which
	     will be fixed at sync but we'd rather not do a global sync and
	     syncing just files doesn't seem to help. Relax the check for
	     this time.  */
#ifdef GRUB_UTIL
	  if (grub_strcmp (p->name, "btrfs") == 0)
	    {
	      char *label = 0;
	      p->fs_uuid (device, &label);
	      if (label)
		grub_free (label);
	    }
	  else
#endif
	    (p->fs_dir) (device, "/", probe_dummy_iter, NULL);
	  if (grub_errno == GRUB_ERR_NONE)
	    {
	      grub_dprintf ("portdbg", "fs_probe: matched fs=%s\n", p->name);
	      return p;
	    }

	  grub_error_push ();
	  /* The grub_error_push() does not touch grub_errmsg. */
	  grub_dprintf ("fs", _("error: %s.\n"), grub_errmsg);
	  grub_dprintf ("fs", "%s detection failed.\n", p->name);
	  grub_error_pop ();

	  if (grub_errno != GRUB_ERR_BAD_FS
	      && grub_errno != GRUB_ERR_OUT_OF_RANGE)
	    {
	      grub_dprintf ("portdbg", "fs_probe: hard fail fs=%s errno=%d msg=%s\n",
			    p->name, grub_errno, grub_errmsg);
	      return 0;
	    }

	  grub_errno = GRUB_ERR_NONE;
	}

      /* Let's load modules automatically.  */
      if (grub_fs_autoload_hook && count == 0)
	{
	  count++;

	  while (grub_fs_autoload_hook ())
	    {
	      p = grub_fs_list;

	      (p->fs_dir) (device, "/", probe_dummy_iter, NULL);
	      if (grub_errno == GRUB_ERR_NONE)
		{
		  count--;
		  return p;
		}

	      if (grub_errno != GRUB_ERR_BAD_FS
		  && grub_errno != GRUB_ERR_OUT_OF_RANGE)
		{
		  count--;
		  return 0;
		}

	      grub_errno = GRUB_ERR_NONE;
	    }

	  count--;
	}
    }
  else if (device->net && device->net->fs)
    return device->net->fs;

  grub_dprintf ("portdbg", "fs_probe: unknown fs (disk=%p net=%p)\n",
		device->disk, device->net);
  grub_error (GRUB_ERR_UNKNOWN_FS, N_("unknown filesystem"));
  return 0;
}



/* Block list support routines.  */

struct grub_fs_block
{
  grub_disk_addr_t offset;
  grub_disk_addr_t length;
};

/* Compatible with grub_alive blocklist syntax:
   - N      : sector N
   - [N]    : byte offset N
   - N[M]   : sector N plus byte offset M
*/
static grub_uint64_t
grub_blocklist_strtooffset (const char *str, const char **end)
{
  grub_uint64_t ret = 0;

  if (*str != '[')
    {
      ret = grub_strtoull (str, &str, 0);
      ret <<= GRUB_DISK_SECTOR_BITS;
    }
  if (*str == '[')
    {
      str++;
      ret += grub_strtoull (str, &str, 0);
    }
  if (*str == ']')
    str++;

  if (end)
    *end = str;
  return ret;
}

static grub_err_t
grub_fs_blocklist_open (grub_file_t file, const char *name)
{
  const char *p = name;
  unsigned num = 0;
  unsigned i;
  grub_disk_t disk = file->device->disk;
  struct grub_fs_block *blocks;
  grub_uint64_t max_sectors;
  grub_uint64_t max_bytes;
  grub_uint64_t part_sectors = 0;

  /* First, count the number of blocks.  */
  do
    {
      num++;
      p = grub_strchr (p, ',');
      if (p)
	p++;
    }
  while (p);

  /* Allocate a block list.  */
  blocks = grub_calloc (num + 1, sizeof (struct grub_fs_block));
  if (! blocks)
    return 0;

  file->size = 0;
  max_sectors = grub_disk_from_native_sector (disk, disk->total_sectors);
  max_bytes = max_sectors << GRUB_DISK_SECTOR_BITS;
  p = (char *) name;
  grub_dprintf ("portdbg", "blocklist_open: spec=`%s' disk=%s\n",
		name ? name : "(null)",
		(disk && disk->name) ? disk->name : "(null)");
  if (!*p)
    {
      blocks[0].offset = 0;
      if (disk->partition)
        {
          part_sectors = grub_disk_from_native_sector (disk, disk->partition->len);
          blocks[0].length = part_sectors << GRUB_DISK_SECTOR_BITS;
        }
      else
        blocks[0].length = max_bytes;
      file->size = blocks[0].length;
    }
  else
    for (i = 0; i < num; i++)
      {
        if (*p != '+')
          {
            blocks[i].offset = grub_blocklist_strtooffset (p, &p);
            if (grub_errno != GRUB_ERR_NONE || *p != '+')
              {
                grub_error (GRUB_ERR_BAD_FILENAME,
                            N_("invalid file name `%s'"), name);
                goto fail;
              }
          }

        p++;
        if (*p == '\0' || *p == ',')
          blocks[i].length = max_bytes - blocks[i].offset;
        else
          blocks[i].length = grub_blocklist_strtooffset (p, &p);

        if (grub_errno != GRUB_ERR_NONE
            || blocks[i].length == 0
            || (*p && *p != ',' && !grub_isspace (*p)))
          {
            grub_error (GRUB_ERR_BAD_FILENAME,
                        N_("invalid file name `%s'"), name);
            goto fail;
          }

        if (max_bytes < blocks[i].offset + blocks[i].length)
          {
            grub_error (GRUB_ERR_BAD_FILENAME, "beyond the total sectors");
            goto fail;
          }

        file->size += blocks[i].length;
        grub_dprintf ("portdbg", "blocklist_open: block[%u] off=0x%llx len=0x%llx\n",
		      i, (unsigned long long) blocks[i].offset,
		      (unsigned long long) blocks[i].length);
        p++;
      }

  file->data = blocks;

  return GRUB_ERR_NONE;

 fail:
  grub_dprintf ("portdbg", "blocklist_open fail: spec=`%s' errno=%d msg=%s\n",
		name ? name : "(null)", grub_errno,
		grub_errmsg);
  grub_free (blocks);
  return grub_errno;
}

static grub_ssize_t
grub_fs_blocklist_read (grub_file_t file, char *buf, grub_size_t len)
{
  struct grub_fs_block *p;
  grub_off_t offset;
  grub_ssize_t ret = 0;
  grub_disk_t disk = file->device->disk;

  if (len > file->size - file->offset)
    len = file->size - file->offset;
  grub_dprintf ("portdbg", "blocklist_read: off=0x%llx len=0x%llx size=0x%llx\n",
		(unsigned long long) file->offset,
		(unsigned long long) len,
		(unsigned long long) file->size);

  offset = file->offset;
  disk->read_hook = file->read_hook;
  disk->read_hook_data = file->read_hook_data;
  for (p = file->data; p->length && len > 0; p++)
    {
      if (offset < p->length)
        {
          grub_size_t size = len;
          grub_disk_addr_t block_offset = p->offset + offset;
          grub_disk_addr_t sector = block_offset >> GRUB_DISK_SECTOR_BITS;
          grub_off_t sec_off = block_offset & (GRUB_DISK_SECTOR_SIZE - 1);

          if (offset + size > p->length)
            size = p->length - offset;

          if (grub_disk_read (disk, sector, sec_off, size, buf) != GRUB_ERR_NONE)
            {
              ret = -1;
              break;
            }

          ret += size;
          len -= size;
          buf += size;
          offset += size;
        }
      else
        offset -= p->length;
    }
  disk->read_hook = NULL;
  disk->read_hook_data = NULL;

  return ret;
}

static grub_err_t
grub_fs_blocklist_close (grub_file_t file)
{
  grub_free (file->data);
  file->data = 0;
  return GRUB_ERR_NONE;
}

struct grub_fs grub_fs_blocklist =
  {
    .name = "blocklist",
    .fs_dir = 0,
    .fs_open = grub_fs_blocklist_open,
    .fs_read = grub_fs_blocklist_read,
    .fs_close = grub_fs_blocklist_close,
    .next = 0
  };
