/* Ordered virtual-disk construction and sparse copy-on-write I/O. */
/*
 * GRUB -- GRand Unified Bootloader
 * Copyright (C) 2026 Free Software Foundation, Inc.
 *
 * GRUB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <grub/disk_pipeline.h>
#include <grub/err.h>
#include <grub/mm.h>
#include <grub/misc.h>

/* Registered once on first construction; common BLOCK hooks are
   independent from GRUB Loopback and from EFI Map publishers. */
void grub_disk_overlay_register (void);
void grub_disk_source_register (void);

static struct grub_disk_hook *grub_disk_hooks;

grub_err_t
grub_disk_hook_register (struct grub_disk_hook *hook, grub_dl_t module)
{
  struct grub_disk_hook **place;

  if (!hook || !hook->run || hook->stage >= GRUB_DISK_STAGE_COUNT)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid disk stage hook");

  for (place = &grub_disk_hooks; *place; place = &(*place)->next)
    {
      if (*place == hook)
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "disk hook already registered");
      if ((*place)->stage > hook->stage
          || ((*place)->stage == hook->stage && (*place)->order > hook->order))
        break;
    }

  hook->module = module;
  hook->next = *place;
  *place = hook;
  return GRUB_ERR_NONE;
}

void
grub_disk_hook_unregister (struct grub_disk_hook *hook)
{
  struct grub_disk_hook **place;

  for (place = &grub_disk_hooks; *place; place = &(*place)->next)
    if (*place == hook)
      {
        *place = hook->next;
        hook->next = 0;
        hook->module = 0;
        return;
      }
}

int
grub_disk_stage_available (enum grub_disk_stage stage)
{
  struct grub_disk_hook *hook;

  for (hook = grub_disk_hooks; hook; hook = hook->next)
    if (hook->stage == stage)
      return 1;
  return 0;
}

/* Ordered VIEW transforms compose from the outside inward. A successful
   transform restarts matching at the first hook; a nonmatch leaves the
   original file untouched. A failing hook must leave ctx->file owned by
   the caller. */
grub_err_t
grub_disk_pipeline_views (struct grub_disk_pipeline *ctx)
{
  unsigned int depth;

  if (!ctx || !ctx->file)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "disk VIEW without source");
  if (!(ctx->flags & GRUB_DISK_PIPELINE_DECOMPRESS)
      || (ctx->type & GRUB_FILE_TYPE_NO_DECOMPRESS))
    return GRUB_ERR_NONE;

  for (depth = 0; depth < 8; depth++)
    {
      struct grub_disk_hook *hook;
      int changed = 0;

      for (hook = grub_disk_hooks; hook; hook = hook->next)
        {
          grub_file_t before;
          grub_err_t err;
          int match;

          if (hook->stage != GRUB_DISK_VIEW)
            continue;
          match = hook->probe ? hook->probe (ctx, hook) : 1;
          if (match < 0)
            return grub_errno ? grub_errno
                              : grub_error (GRUB_ERR_BAD_FILE_TYPE,
                                            "disk VIEW probe failed");
          if (!match)
            continue;

          before = ctx->file;
          err = hook->run (ctx, hook);
          if (err)
            return err;
          if (!ctx->file)
            return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                               "disk VIEW hook returned no file");
          if (before != ctx->file)
            {
              ctx->transformed = 1;
              ctx->direct_write = 0;
              changed = 1;
              break;
            }
        }
      if (!changed)
        return GRUB_ERR_NONE;
    }

  return grub_error (GRUB_ERR_BAD_COMPRESSED_DATA,
                     "too many nested disk compression layers");
}

static grub_err_t grub_disk_pipeline_read_base_layer
  (struct grub_disk_layer *, grub_uint64_t, grub_size_t, void *);
static grub_err_t grub_disk_pipeline_write_base_layer
  (struct grub_disk_layer *, grub_uint64_t, grub_size_t, const void *);

struct grub_disk_pipeline *
grub_disk_pipeline_new (const char *source, const char *name,
                        enum grub_disk_target target, unsigned int flags)
{
  struct grub_disk_pipeline *ctx;

  if (!source || !name)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "disk source and name are required");
      return 0;
    }

  ctx = grub_zalloc (sizeof (*ctx));
  if (!ctx)
    return 0;
  ctx->source = grub_strdup (source);
  ctx->name = grub_strdup (name);
  if (!ctx->source || !ctx->name)
    {
      grub_disk_pipeline_close (ctx);
      return 0;
    }
  static int builtin_hooks_registered;
  if (!builtin_hooks_registered)
    {
      grub_disk_source_register ();
      grub_disk_overlay_register ();
      builtin_hooks_registered = 1;
    }
  ctx->target = target;
  ctx->flags = flags;
  ctx->type = GRUB_FILE_TYPE_LOOPBACK;
  return ctx;
}

grub_err_t
grub_disk_pipeline_run (struct grub_disk_pipeline *ctx)
{
  struct grub_disk_hook *hook;
  unsigned int stage;

  if (!ctx || ctx->file || ctx->published)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid disk pipeline state");

  for (stage = GRUB_DISK_SOURCE; stage < GRUB_DISK_STAGE_COUNT; stage++)
    {
      int selected = 0;
      int exclusive = (stage == GRUB_DISK_SOURCE || stage == GRUB_DISK_FORMAT
                       || stage == GRUB_DISK_PUBLISH);

      if (stage == GRUB_DISK_BLOCK)
        {
          ctx->base.data = ctx;
          ctx->base.read = grub_disk_pipeline_read_base_layer;
          ctx->base.write = grub_disk_pipeline_write_base_layer;
          ctx->io = &ctx->base;
        }
      if (stage == GRUB_DISK_VIEW)
        {
          grub_err_t err = grub_disk_pipeline_views (ctx);
          if (err)
            return err;
          continue;
        }
      if (stage == GRUB_DISK_FORMAT
          && !(ctx->flags & GRUB_DISK_PIPELINE_FORMAT))
        continue;

      for (hook = grub_disk_hooks; hook; hook = hook->next)
        {
          grub_err_t err;

          if (hook->stage != stage)
            continue;
          if (hook->probe)
            {
              int match = hook->probe (ctx, hook);
              if (match < 0)
                return grub_errno ? grub_errno
                                  : grub_error (GRUB_ERR_BAD_FILE_TYPE,
                                                "disk hook probe failed");
              if (!match)
                continue;
            }

          grub_dprintf ("diskpipe", "stage=%u hook=%s\n", stage, hook->name);
          err = hook->run (ctx, hook);
          if (err != GRUB_ERR_NONE)
            return err;
          selected = 1;

          if (stage == GRUB_DISK_FORMAT)
            {
              if (!ctx->formatted || !ctx->file)
                return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                                   "format hook did not produce a disk");
              if (hook->module)
                {
                  grub_dl_ref (hook->module);
                  ctx->format_module = hook->module;
                }
            }
          if (stage == GRUB_DISK_PUBLISH)
            {
              if (!ctx->published)
                return grub_error (GRUB_ERR_BAD_DEVICE,
                                   "disk publish hook produced no device");
              ctx->publisher = hook;
              if (hook->module)
                grub_dl_ref (hook->module);
            }
          if (exclusive)
            break;
        }

      if ((stage == GRUB_DISK_SOURCE || stage == GRUB_DISK_PUBLISH
           || stage == GRUB_DISK_FORMAT) && !selected)
        return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                           "no hook accepted disk stage %u", stage);
      if (stage == GRUB_DISK_SOURCE && !ctx->file)
        return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                           "disk source hook produced no file");
      if (stage == GRUB_DISK_PUBLISH && !ctx->published)
        return grub_error (GRUB_ERR_BAD_DEVICE,
                           "disk publish hook produced no device");
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_disk_pipeline_read_base (struct grub_disk_pipeline *ctx,
                              grub_uint64_t off, grub_size_t len, void *buf)
{
  grub_ssize_t got;
  grub_size_t available = len;

  if (!ctx->file)
    return grub_error (GRUB_ERR_BAD_DEVICE, "disk source is closed");

  if (ctx->file->size != GRUB_FILE_SIZE_UNKNOWN)
    {
      if (off >= ctx->file->size)
        available = 0;
      else if (available > ctx->file->size - off)
        available = ctx->file->size - off;
    }

  if (available)
    {
      if (grub_file_seek (ctx->file, off) == (grub_off_t) -1)
        return grub_errno ? grub_errno
                          : grub_error (GRUB_ERR_OUT_OF_RANGE,
                                        "disk source seek failed");
      got = grub_file_read (ctx->file, buf, available);
      if (got != (grub_ssize_t) available)
        return grub_errno ? grub_errno
                          : grub_error (GRUB_ERR_READ_ERROR,
                                        "short read from disk source");
    }

  if (available != len)
    grub_memset ((grub_uint8_t *) buf + available, 0, len - available);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_disk_pipeline_read_base_layer (struct grub_disk_layer *layer,
                                    grub_uint64_t off, grub_size_t len, void *buf)
{
  return grub_disk_pipeline_read_base (layer->data, off, len, buf);
}

static grub_err_t
grub_disk_pipeline_write_base_layer (struct grub_disk_layer *layer,
                                     grub_uint64_t off, grub_size_t len,
                                     const void *buf)
{
  struct grub_disk_pipeline *ctx = layer->data;
  if (!ctx->direct_write)
    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET, "disk backing is read-only");
  return ctx->direct_write (ctx->file, buf, len, off);
}

grub_err_t
grub_disk_pipeline_read (struct grub_disk_pipeline *ctx,
                         grub_uint64_t off, grub_size_t len, void *buf)
{
  if (!ctx || !ctx->io || !buf)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid disk read");
  if (!len)
    return GRUB_ERR_NONE;
  if (off > (grub_uint64_t) -1 - len)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "disk read offset overflow");
  return ctx->io->read (ctx->io, off, len, buf);
}

grub_err_t
grub_disk_pipeline_write (struct grub_disk_pipeline *ctx,
                          grub_uint64_t off, grub_size_t len, const void *buf)
{
  if (!ctx || !ctx->io || !buf)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid disk write");
  if (!len)
    return GRUB_ERR_NONE;
  if (ctx->flags & GRUB_DISK_PIPELINE_READ_ONLY)
    return grub_error (GRUB_ERR_WRITE_ERROR, "disk is read-only");
  if (ctx->file->size != GRUB_FILE_SIZE_UNKNOWN
      && (off > ctx->file->size || len > ctx->file->size - off))
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "disk write outside image");
  return ctx->io->write (ctx->io, off, len, buf);
}

grub_err_t
grub_disk_pipeline_close (struct grub_disk_pipeline *ctx)
{
  if (!ctx)
    return GRUB_ERR_NONE;

  /* The publisher must remove its device before the backing I/O chain dies. */
  if (ctx->publisher)
    {
      const struct grub_disk_hook *publisher = ctx->publisher;
      ctx->publisher = 0;
      if (publisher->unpublish)
        {
          grub_err_t err = publisher->unpublish (ctx, publisher);
          if (err)
            {
              ctx->publisher = publisher;
              return err;
            }
        }
      if (publisher->module)
        grub_dl_unref (publisher->module);
    }

  /* Reverse the assembled chain before destroying backing views. */
  while (ctx->io && ctx->io != &ctx->base)
    {
      struct grub_disk_layer *top = ctx->io;
      ctx->io = top->lower;
      if (top->close)
        top->close (top);
    }

  if (ctx->file)
    grub_file_close (ctx->file);
  /* Close the entire file view chain before freeing copied source memory. */
  if (ctx->owned_memory)
    {
      if (ctx->release_memory)
        ctx->release_memory (ctx, ctx->owned_memory, ctx->owned_memory_size);
      else
        grub_free (ctx->owned_memory);
    }
  if (ctx->format_module)
    grub_dl_unref (ctx->format_module);
  grub_free (ctx->source);
  grub_free (ctx->name);
  grub_free (ctx);
  return GRUB_ERR_NONE;
}
