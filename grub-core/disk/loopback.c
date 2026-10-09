/* File-backed GRUB disks published by the ordered disk pipeline. */
/*
 * GRUB -- GRand Unified Bootloader
 * Copyright (C) 2005,2006,2007,2026 Free Software Foundation, Inc.
 *
 * GRUB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <grub/disk.h>
#include <grub/disk_pipeline.h>
#include <grub/dl.h>
#include <grub/extcmd.h>
#include <grub/file.h>
#include <grub/fileview.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/safemath.h>

GRUB_MOD_LICENSE ("GPLv3+");

struct grub_loopback
{
  struct grub_disk_pipeline *pipeline;
  struct grub_loopback *next;
  unsigned long id;
  grub_uint64_t refcnt;
};

static struct grub_loopback *loopback_list;
static unsigned long last_id;
static grub_dl_t loopback_module;

static const struct grub_arg_option options[] =
  {
    {"delete", 'd', 0, N_("Delete the specified loopback drive."), 0, 0},
    {"mem", 'm', 0, N_("Copy to RAM."), 0, 0},
    {"blocklist", 'l', 0, N_("Convert to blocklist."), 0, 0},
    {"decompress", 'D', 0,
     N_("Compatibility option; backing files are decompressed automatically."),
     0, 0},
    {"cow", 'C', 0, N_("Keep writes in a volatile copy-on-write overlay."), 0, 0},
    {0, 0, 0, 0, 0, 0}
  };

static int
grub_loopback_stage_match (struct grub_disk_pipeline *ctx,
                           const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  return ctx->target == GRUB_DISK_TARGET_LOOPBACK;
}

static grub_err_t
grub_loopback_stage_publish (struct grub_disk_pipeline *ctx,
                             const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  struct grub_loopback *dev;
  grub_disk_t existing;

  for (dev = loopback_list; dev; dev = dev->next)
    if (grub_strcmp (dev->pipeline->name, ctx->name) == 0)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "device name already exists");

  /* The GRUB namespace also contains firmware disks and reserved devices,
     such as the kernel-owned 'mem' disk.  Do not silently shadow one. */
  existing = grub_disk_open (ctx->name);
  if (existing)
    {
      grub_disk_close (existing);
      return grub_error (GRUB_ERR_BAD_ARGUMENT,
                         "disk name already belongs to another device");
    }
  if (grub_errno != GRUB_ERR_NONE && grub_errno != GRUB_ERR_UNKNOWN_DEVICE)
    return grub_errno;
  grub_errno = GRUB_ERR_NONE;

  dev = grub_zalloc (sizeof (*dev));
  if (!dev)
    return grub_errno;

  dev->pipeline = ctx;
  dev->id = last_id++;
  dev->next = loopback_list;
  loopback_list = dev;
  ctx->published = 1;
  if (loopback_module)
    grub_dl_ref (loopback_module);
  return GRUB_ERR_NONE;
}

static struct grub_disk_hook grub_loopback_hooks[] =
  {
    { .name = "grub-loopback", .stage = GRUB_DISK_PUBLISH,
      .probe = grub_loopback_stage_match, .run = grub_loopback_stage_publish }
  };

static grub_err_t
delete_loopback (const char *name)
{
  struct grub_loopback *dev;
  struct grub_loopback **prev;

  for (dev = loopback_list, prev = &loopback_list;
       dev; prev = &dev->next, dev = dev->next)
    if (grub_strcmp (dev->pipeline->name, name) == 0)
      break;

  if (!dev)
    return grub_error (GRUB_ERR_BAD_DEVICE, "device not found");
  if (dev->refcnt)
    return grub_error (GRUB_ERR_STILL_REFERENCED, "device still referenced");

  *prev = dev->next;
  grub_disk_pipeline_close (dev->pipeline);
  grub_free (dev);
  if (loopback_module)
    grub_dl_unref (loopback_module);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_loopback_create (grub_extcmd_context_t ctxt, int argc, char **args,
                          int format)
{
  struct grub_arg_list *state = ctxt->state;
  struct grub_disk_pipeline *ctx;
  struct grub_loopback *dev;
  /* Keep the existing fork's transparent decompression behavior, including
     grubfm mappings that omit -D.  -D remains accepted for compatibility. */
  unsigned int flags = GRUB_DISK_PIPELINE_DECOMPRESS;
  grub_err_t err;

  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "device name required");
  if (state[0].set)
    return delete_loopback (args[0]);
  if (argc < 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  for (dev = loopback_list; dev; dev = dev->next)
    if (grub_strcmp (dev->pipeline->name, args[0]) == 0)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "device name already exists");

  if (format)
    {
      if (!grub_disk_stage_available (GRUB_DISK_FORMAT))
        {
#ifndef GRUB_UTIL
          if (!grub_dl_load ("vhd"))
            return grub_errno;
#else
          return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                             "no virtual disk parser registered");
#endif
        }
      flags |= GRUB_DISK_PIPELINE_FORMAT;
    }
  if (state[1].set)
    flags |= GRUB_DISK_PIPELINE_MEMORY;
  if (state[2].set)
    flags |= GRUB_DISK_PIPELINE_BLOCKLIST;
  if (state[4].set)
    flags |= GRUB_DISK_PIPELINE_COW;

  ctx = grub_disk_pipeline_new (args[1], args[0],
                               GRUB_DISK_TARGET_LOOPBACK, flags);
  if (!ctx)
    return grub_errno;

  err = grub_disk_pipeline_run (ctx);
  if (err)
    {
      grub_disk_pipeline_close (ctx);
      grub_errno = err;
    }
  return err;
}

static grub_err_t
grub_cmd_loopback (grub_extcmd_context_t ctxt, int argc, char **args)
{
  return grub_cmd_loopback_create (ctxt, argc, args, 0);
}

static grub_err_t
grub_cmd_vhd (grub_extcmd_context_t ctxt, int argc, char **args)
{
  return grub_cmd_loopback_create (ctxt, argc, args, 1);
}

static int
grub_loopback_iterate (grub_disk_dev_iterate_hook_t hook, void *hook_data,
                       grub_disk_pull_t pull)
{
  struct grub_loopback *dev;

  if (pull != GRUB_DISK_PULL_NONE)
    return 0;
  for (dev = loopback_list; dev; dev = dev->next)
    if (hook (dev->pipeline->name, hook_data))
      return 1;
  return 0;
}

static grub_err_t
grub_loopback_open (const char *name, grub_disk_t disk)
{
  struct grub_loopback *dev;
  grub_file_t file;

  for (dev = loopback_list; dev; dev = dev->next)
    if (grub_strcmp (dev->pipeline->name, name) == 0)
      break;
  if (!dev)
    return grub_error (GRUB_ERR_UNKNOWN_DEVICE, "can't open device");

  if (grub_add (dev->refcnt, 1, &dev->refcnt))
    grub_fatal ("Reference count overflow");

  file = dev->pipeline->file;
  if (file->log_sector_size >= GRUB_DISK_SECTOR_BITS)
    disk->log_sector_size = file->log_sector_size;

  if (file->size != GRUB_FILE_SIZE_UNKNOWN)
    {
      grub_uint64_t sector_size = 1ULL << disk->log_sector_size;
      disk->total_sectors = (file->size + sector_size - 1) / sector_size;
    }
  else
    disk->total_sectors = GRUB_DISK_SIZE_UNKNOWN;

  disk->max_agglomerate = 1 << (29 - GRUB_DISK_SECTOR_BITS
                               - GRUB_DISK_CACHE_BITS);
  disk->id = dev->id;
  disk->data = dev;
  return GRUB_ERR_NONE;
}

static void
grub_loopback_close (grub_disk_t disk)
{
  struct grub_loopback *dev = disk->data;
  if (grub_sub (dev->refcnt, 1, &dev->refcnt))
    grub_fatal ("Reference count underflow");
}

static grub_err_t
grub_loopback_read (grub_disk_t disk, grub_disk_addr_t sector,
                    grub_size_t size, char *buf)
{
  struct grub_loopback *dev = disk->data;
  return grub_disk_pipeline_read (dev->pipeline,
                                  (grub_uint64_t) sector << disk->log_sector_size,
                                  size << disk->log_sector_size, buf);
}

static grub_err_t
grub_loopback_write (grub_disk_t disk, grub_disk_addr_t sector,
                     grub_size_t size, const char *buf)
{
  struct grub_loopback *dev = disk->data;
  return grub_disk_pipeline_write (dev->pipeline,
                                   (grub_uint64_t) sector << disk->log_sector_size,
                                   size << disk->log_sector_size, buf);
}

static struct grub_disk_dev grub_loopback_dev =
  {
    .name = "loopback",
    .id = GRUB_DISK_DEVICE_LOOPBACK_ID,
    .disk_iterate = grub_loopback_iterate,
    .disk_open = grub_loopback_open,
    .disk_close = grub_loopback_close,
    .disk_read = grub_loopback_read,
    .disk_write = grub_loopback_write,
    .next = 0
  };

static grub_extcmd_t cmd;
static grub_extcmd_t cmd_vhd;

GRUB_MOD_INIT(loopback)
{
  grub_size_t i;

  loopback_module = mod;
  for (i = 0; i < ARRAY_SIZE (grub_loopback_hooks); i++)
    grub_disk_hook_register (&grub_loopback_hooks[i], mod);

  cmd = grub_register_extcmd ("loopback", grub_cmd_loopback, 0,
                              N_("[-d] [-m] [-l] [-D] [-C] DEVICENAME FILE."),
                              N_("Make a virtual drive from a file."), options);
  cmd_vhd = grub_register_extcmd ("vhd", grub_cmd_vhd, 0,
                                 N_("[-d] [-m] [-l] [-D] [-C] DEVICENAME FILE."),
                                 N_("Map a virtual disk image."), options);
  grub_disk_dev_register (&grub_loopback_dev);
}

GRUB_MOD_FINI(loopback)
{
  grub_size_t i;

  grub_unregister_extcmd (cmd);
  grub_unregister_extcmd (cmd_vhd);
  grub_disk_dev_unregister (&grub_loopback_dev);
  for (i = 0; i < ARRAY_SIZE (grub_loopback_hooks); i++)
    grub_disk_hook_unregister (&grub_loopback_hooks[i]);
  loopback_module = 0;
}
