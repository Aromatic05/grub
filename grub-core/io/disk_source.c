/* File-backed loopback SOURCE operations. */
/*
 * GRUB -- GRand Unified Bootloader
 * Copyright (C) 2026 Free Software Foundation, Inc.
 *
 * GRUB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <grub/disk.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/disk_pipeline.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/port_write.h>

static grub_file_t
disk_source_open (struct grub_disk_pipeline *ctx)
{
  const char *name = ctx->source;
  int mem = !!(ctx->flags & GRUB_DISK_PIPELINE_MEMORY);
  int blocklist = !!(ctx->flags & GRUB_DISK_PIPELINE_BLOCKLIST);
  enum grub_file_type type = ctx->type | GRUB_FILE_TYPE_NO_DECOMPRESS;
  grub_file_t file;
  grub_size_t size;

  file = grub_file_open (name, type);
  if (!file)
    return 0;

  size = grub_file_size (file);
  if (blocklist && !grub_port_file_prepare_write (file))
    {
      grub_file_close (file);
      return 0;
    }

  if (mem)
    {
      void *addr;
      char newname[100];
      grub_ssize_t readlen;

      if (size == GRUB_FILE_SIZE_UNKNOWN
          || size > (grub_size_t) -1 - (GRUB_DISK_SECTOR_SIZE - 1))
        {
          grub_file_close (file);
          grub_error (GRUB_ERR_OUT_OF_RANGE,
                      "cannot copy unknown or oversized disk to RAM");
          return 0;
        }

      addr = ctx->allocate_memory
        ? ctx->allocate_memory (ctx, size + GRUB_DISK_SECTOR_SIZE - 1)
        : grub_malloc (size + GRUB_DISK_SECTOR_SIZE - 1);
      if (!addr)
        {
          grub_file_close (file);
          return 0;
        }

      if (grub_file_seek (file, 0) == (grub_off_t) -1)
        {
          if (ctx->release_memory)
            ctx->release_memory (ctx, addr, size + GRUB_DISK_SECTOR_SIZE - 1);
          else
            grub_free (addr);
          grub_file_close (file);
          return 0;
        }
      readlen = grub_file_read (file, addr, size);
      grub_file_close (file);
      if (readlen < 0 || (grub_size_t) readlen != size)
        {
          if (ctx->release_memory)
            ctx->release_memory (ctx, addr, size + GRUB_DISK_SECTOR_SIZE - 1);
          else
            grub_free (addr);
          return 0;
        }

      grub_memset ((grub_uint8_t *) addr + size, 0,
                   GRUB_DISK_SECTOR_SIZE - 1);
      grub_snprintf (newname, sizeof (newname), "mem:%p:size:%llu",
                     addr, (unsigned long long) size);
      file = grub_file_open (newname, type);
      if (!file)
        {
          if (ctx->release_memory)
            ctx->release_memory (ctx, addr, size + GRUB_DISK_SECTOR_SIZE - 1);
          else
            grub_free (addr);
        }
      else
        {
          ctx->owned_memory = addr;
          ctx->owned_memory_size = size + GRUB_DISK_SECTOR_SIZE - 1;
        }
    }

  return file;
}

static grub_err_t
disk_source_write (grub_file_t file, const void *buf, grub_size_t len,
                          grub_off_t offset)
{
  grub_ssize_t written;

  if (!file)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid loopback file");
  if (!grub_port_file_prepare_write (file))
    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                       "direct write supports mem: or disk-backed files only");

#ifndef GRUB_UTIL
  if (!grub_port_is_mem_name (file->name) && !grub_disk_write_weak)
    {
      if (!grub_dl_load ("disk"))
        return grub_errno ? grub_errno
                          : grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                                        "disk write module unavailable");
    }
#endif

  if (grub_file_seek (file, offset) == (grub_off_t) -1)
    return grub_errno;
  written = grub_port_file_write (file, (const char *) buf, len);
  if (written < 0)
    return grub_errno ? grub_errno
                      : grub_error (GRUB_ERR_WRITE_ERROR,
                                    "loopback write failed");
  if ((grub_size_t) written != len)
    return grub_error (GRUB_ERR_WRITE_ERROR, "short write to loopback file");
  return GRUB_ERR_NONE;
}


static grub_err_t
disk_source_stage (struct grub_disk_pipeline *ctx,
                   const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  ctx->file = disk_source_open (ctx);
  if (!ctx->file)
    return grub_errno ? grub_errno
                      : grub_error (GRUB_ERR_FILE_NOT_FOUND,
                                    "could not open disk source");
  ctx->direct_write = disk_source_write;
  return GRUB_ERR_NONE;
}

void
grub_disk_source_register (void)
{
  static struct grub_disk_hook hook =
    { .name = "file", .stage = GRUB_DISK_SOURCE, .order = 0,
      .run = disk_source_stage };
  grub_disk_hook_register (&hook, 0);
}
