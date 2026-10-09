/* vdisk.c - shared helpers for ported virtual-disk parsers.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2026  Free Software Foundation, Inc.
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

#include <grub/err.h>
#include <grub/file.h>
#include <grub/misc.h>
#include <grub/vdisk.h>

static const struct grub_vdisk_parser_desc *grub_vdisk_parsers[GRUB_FILE_FILTER_MAX];
static const grub_file_filter_id_t grub_vdisk_parser_order[] =
  {
    GRUB_FILE_FILTER_QCOW2IO,
    GRUB_FILE_FILTER_VHDXIO,
    GRUB_FILE_FILTER_VMDKIO,
    GRUB_FILE_FILTER_FIXED_VDIIO,
    GRUB_FILE_FILTER_VHDIO
  };

static grub_ssize_t grub_vdisk_file_read (grub_file_t file, char *buf,
                                          grub_size_t len);
static grub_err_t grub_vdisk_file_close (grub_file_t file);
static struct grub_fs grub_vdisk_fs =
  {
    .name = "vdisk",
    .fs_dir = 0,
    .fs_open = 0,
    .fs_read = grub_vdisk_file_read,
    .fs_close = grub_vdisk_file_close,
    .fs_label = 0,
    .next = 0
  };

int
grub_vdisk_filter_should_open (grub_file_t io, enum grub_file_type type,
                               grub_off_t min_size)
{
  if (!io)
    return 0;
  if ((type & GRUB_FILE_TYPE_MASK) != GRUB_FILE_TYPE_LOOPBACK)
    return 0;
  if (type & GRUB_FILE_TYPE_NO_DECOMPRESS)
    return 0;
  if (io->size < min_size)
    return 0;
  return 1;
}

int
grub_vdisk_read_exact (grub_file_t file, grub_off_t off,
                       void *buf, grub_size_t len)
{
  grub_ssize_t got;

  grub_file_seek (file, off);
  if (grub_errno != GRUB_ERR_NONE)
    return 0;

  got = grub_file_read (file, buf, len);
  if (got < 0 || (grub_size_t) got != len)
    return 0;

  return 1;
}

grub_file_t
grub_vdisk_create (grub_size_t object_size, struct grub_vdisk **disk_out)
{
  grub_file_t file;
  struct grub_vdisk *disk;

  if (disk_out)
    *disk_out = 0;

  file = grub_zalloc (sizeof (*file));
  if (!file)
    return 0;

  disk = grub_zalloc (object_size);
  if (!disk)
    {
      grub_free (file);
      return 0;
    }

  if (disk_out)
    *disk_out = disk;
  return file;
}

grub_file_t
grub_vdisk_open (grub_size_t object_size, struct grub_vdisk **disk_out,
                 grub_file_t backing, grub_off_t size,
                 grub_uint32_t log_sector_size, grub_vdisk_read_t read,
                 grub_vdisk_destroy_t destroy, const char *name)
{
  grub_file_t file;
  struct grub_vdisk *disk;

  file = grub_vdisk_create (object_size, &disk);
  if (!file)
    return 0;

  grub_vdisk_init (disk, backing, size, log_sector_size, read, destroy, name);
  grub_vdisk_attach_object (file, disk);

  if (disk_out)
    *disk_out = disk;
  return file;
}

void
grub_vdisk_fail (grub_file_t file, struct grub_vdisk *disk)
{
  if (disk)
    {
      if (disk->destroy)
        disk->destroy (disk);
      else
        grub_free (disk);
    }
  grub_free (file);
}

void
grub_vdisk_attach (grub_file_t file, grub_file_t backing, void *data,
                   struct grub_fs *fs, grub_off_t size,
                   grub_uint32_t log_sector_size)
{
  file->device = backing->device;
  file->data = data;
  file->fs = fs;
  file->size = size;
  file->log_sector_size = log_sector_size;
  file->not_easily_seekable = backing->not_easily_seekable;
}

void
grub_vdisk_init (struct grub_vdisk *disk, grub_file_t backing,
                 grub_off_t size, grub_uint32_t log_sector_size,
                 grub_vdisk_read_t read, grub_vdisk_destroy_t destroy,
                 const char *name)
{
  disk->backing = backing;
  disk->size = size;
  disk->log_sector_size = log_sector_size;
  disk->read = read;
  disk->destroy = destroy;
  disk->name = name;
}

void
grub_vdisk_attach_object (grub_file_t file, struct grub_vdisk *disk)
{
  grub_vdisk_attach (file, disk->backing, disk, &grub_vdisk_fs,
                     disk->size, disk->log_sector_size);
}

static grub_ssize_t
grub_vdisk_file_read (grub_file_t file, char *buf, grub_size_t len)
{
  struct grub_vdisk *disk = file->data;

  /* grub_file_read() advances file->offset after fs_read returns. */
  return disk->read (disk, file->offset, buf, len);
}

static grub_err_t
grub_vdisk_file_close (grub_file_t file)
{
  struct grub_vdisk *disk = file->data;

  if (disk && disk->destroy)
    disk->destroy (disk);

  file->device = 0;
  file->name = 0;
  return grub_errno;
}

void
grub_vdisk_register_parser (const struct grub_vdisk_parser_desc *parser)
{
  grub_file_filter_id_t id;

  if (!parser)
    return;

  id = parser->id;
  if (id <= GRUB_FILE_FILTER_ZSTDIO || id >= GRUB_FILE_FILTER_MAX)
    return;
  grub_vdisk_parsers[id] = parser;
}

void
grub_vdisk_unregister_parser (grub_file_filter_id_t id)
{
  if (id <= GRUB_FILE_FILTER_ZSTDIO || id >= GRUB_FILE_FILTER_MAX)
    return;
  grub_vdisk_parsers[id] = 0;
}

void
grub_vdisk_register_parsers (const struct grub_vdisk_parser_desc *parsers,
                             grub_size_t count)
{
  grub_size_t i;

  for (i = 0; i < count; i++)
    grub_vdisk_register_parser (&parsers[i]);
}

void
grub_vdisk_unregister_parsers (const struct grub_vdisk_parser_desc *parsers,
                               grub_size_t count)
{
  grub_size_t i;

  for (i = 0; i < count; i++)
    grub_vdisk_unregister_parser (parsers[i].id);
}

int
grub_vdisk_parsers_ready (void)
{
  grub_size_t i;

  for (i = 0; i < ARRAY_SIZE (grub_vdisk_parser_order); i++)
    if (grub_vdisk_parsers[grub_vdisk_parser_order[i]])
      return 1;
  return 0;
}

const char *
grub_vdisk_parser_name (grub_file_filter_id_t id)
{
  if (id <= GRUB_FILE_FILTER_ZSTDIO || id >= GRUB_FILE_FILTER_MAX)
    return 0;
  return grub_vdisk_parsers[id] ? grub_vdisk_parsers[id]->name : 0;
}

grub_file_t
grub_vdisk_apply_parsers (grub_file_t io, enum grub_file_type type)
{
  grub_size_t i;
  grub_file_t next;

  if (!io)
    return 0;

  for (i = 0; i < ARRAY_SIZE (grub_vdisk_parser_order); i++)
    {
      grub_file_filter_id_t id;
      const struct grub_vdisk_parser_desc *parser;

      id = grub_vdisk_parser_order[i];
      parser = grub_vdisk_parsers[id];
      if (!parser)
        continue;

      grub_dprintf ("vdiskdbg", "vdisk: try parser=%s file=%p size=%llu\n",
                    grub_vdisk_parser_name (id),
                    io, (unsigned long long) io->size);
      if (parser->probe && !parser->probe (io, type))
        {
          grub_dprintf ("vdiskdbg", "vdisk: parser=%s rejected file=%p\n",
                        grub_vdisk_parser_name (id), io);
          continue;
        }

      grub_dprintf ("vdiskdbg", "vdisk: parser=%s probe matched\n",
                    grub_vdisk_parser_name (id));
      next = parser->open ? parser->open (io, type) : 0;
      if (!next)
        return 0;
      if (next == io)
        {
          grub_error (GRUB_ERR_BAD_FILE_TYPE,
                      "vdisk parser %s returned raw file after probe",
                      grub_vdisk_parser_name (id));
          return 0;
        }

      /* Only a completed parser takes ownership of the input file. */
      ((struct grub_vdisk *) next->data)->owns_backing = 1;

      grub_dprintf ("vdiskdbg",
                    "vdisk: parser=%s accepted size=%llu log_sector=%u\n",
                    grub_vdisk_parser_name (id),
                    (unsigned long long) next->size,
                    next->log_sector_size);
      return next;
    }

  grub_dprintf ("vdiskdbg", "vdisk: no parser matched file=%p size=%llu\n",
                io, (unsigned long long) io->size);
  return io;
}
