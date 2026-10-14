/* Image-format hooks for the ordered disk construction pipeline. */
/*
 * GRUB -- GRand Unified Bootloader
 * Copyright (C) 2026 Free Software Foundation, Inc.
 *
 * GRUB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <grub/dl.h>
#include <grub/disk_pipeline.h>
#include <grub/vdisk.h>

GRUB_MOD_LICENSE ("GPLv3+");

int grub_fixed_vdiio_probe (grub_file_t io, enum grub_file_type type);
grub_file_t grub_fixed_vdiio_open_filter (grub_file_t io, enum grub_file_type type);
int grub_vhdxio_probe (grub_file_t io, enum grub_file_type type);
grub_file_t grub_vhdxio_open_filter (grub_file_t io, enum grub_file_type type);
int grub_vmdkio_probe (grub_file_t io, enum grub_file_type type);
grub_file_t grub_vmdkio_open_filter (grub_file_t io, enum grub_file_type type);
int grub_qcow2io_probe (grub_file_t io, enum grub_file_type type);
grub_file_t grub_qcow2io_open_filter (grub_file_t io, enum grub_file_type type);
int grub_vhdio_probe (grub_file_t io, enum grub_file_type type);
grub_file_t grub_vhdio_open_filter (grub_file_t io, enum grub_file_type type);

static const struct grub_vdisk_parser_desc grub_vdisk_formats[] =
  {
    { "qcow2", grub_qcow2io_probe, grub_qcow2io_open_filter },
    { "vhdx", grub_vhdxio_probe, grub_vhdxio_open_filter },
    { "vmdk", grub_vmdkio_probe, grub_vmdkio_open_filter },
    { "fixed_vdi", grub_fixed_vdiio_probe, grub_fixed_vdiio_open_filter },
    { "vhd", grub_vhdio_probe, grub_vhdio_open_filter }
  };

static int
grub_vdisk_stage_probe (struct grub_disk_pipeline *ctx,
                       const struct grub_disk_hook *hook)
{
  const struct grub_vdisk_parser_desc *parser = hook->data;
  return parser->probe ? parser->probe (ctx->file, ctx->type) : 1;
}

static grub_err_t
grub_vdisk_stage_open (struct grub_disk_pipeline *ctx,
                      const struct grub_disk_hook *hook)
{
  const struct grub_vdisk_parser_desc *parser = hook->data;
  grub_file_t file = parser->open ? parser->open (ctx->file, ctx->type) : 0;

  if (!file)
    return grub_errno ? grub_errno
                      : grub_error (GRUB_ERR_BAD_FILE_TYPE,
                                    "could not open %s image", parser->name);
  if (file == ctx->file)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                       "%s parser returned the backing file", parser->name);

  ((struct grub_vdisk *) file->data)->owns_backing = 1;
  ctx->file = file;
  ctx->formatted = 1;
  ctx->transformed = 1;
  ctx->direct_write = 0;
  return GRUB_ERR_NONE;
}

static struct grub_disk_hook grub_vdisk_hooks[] =
  {
    { .name = "qcow2", .stage = GRUB_DISK_FORMAT, .order = 10,
      .probe = grub_vdisk_stage_probe, .run = grub_vdisk_stage_open },
    { .name = "vhdx", .stage = GRUB_DISK_FORMAT, .order = 20,
      .probe = grub_vdisk_stage_probe, .run = grub_vdisk_stage_open },
    { .name = "vmdk", .stage = GRUB_DISK_FORMAT, .order = 30,
      .probe = grub_vdisk_stage_probe, .run = grub_vdisk_stage_open },
    { .name = "fixed_vdi", .stage = GRUB_DISK_FORMAT, .order = 40,
      .probe = grub_vdisk_stage_probe, .run = grub_vdisk_stage_open },
    { .name = "vhd", .stage = GRUB_DISK_FORMAT, .order = 50,
      .probe = grub_vdisk_stage_probe, .run = grub_vdisk_stage_open }
  };

/* a1ive `vhd` was an alias of `loopback`, and `loopback` mapped unrecognized
   files verbatim.  Keep that contract: once every real container parser has
   declined, expose the source unchanged instead of failing the FORMAT stage.
   The hook runs last and only for GRUB Loopback, so the EFI `map --format`
   stage still reports an unrecognized container. */
static int
grub_vdisk_raw_probe (struct grub_disk_pipeline *ctx,
                      const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  return ctx->target == GRUB_DISK_TARGET_LOOPBACK;
}

static grub_err_t
grub_vdisk_raw_open (struct grub_disk_pipeline *ctx,
                     const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  /* No transform: the source file, its size and direct writes stay as-is. */
  ctx->formatted = 1;
  return GRUB_ERR_NONE;
}

static struct grub_disk_hook grub_vdisk_raw_hook =
  { .name = "raw", .stage = GRUB_DISK_FORMAT, .order = 1000,
    .probe = grub_vdisk_raw_probe, .run = grub_vdisk_raw_open };

GRUB_MOD_INIT(vhd)
{
  grub_size_t i;
  for (i = 0; i < ARRAY_SIZE (grub_vdisk_hooks); i++)
    {
      grub_vdisk_hooks[i].data = &grub_vdisk_formats[i];
      grub_disk_hook_register (&grub_vdisk_hooks[i], mod);
    }
  grub_disk_hook_register (&grub_vdisk_raw_hook, mod);
}

GRUB_MOD_FINI(vhd)
{
  grub_size_t i;
  grub_disk_hook_unregister (&grub_vdisk_raw_hook);
  for (i = 0; i < ARRAY_SIZE (grub_vdisk_hooks); i++)
    grub_disk_hook_unregister (&grub_vdisk_hooks[i]);
}
