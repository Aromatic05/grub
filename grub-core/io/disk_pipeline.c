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

#define GRUB_DISK_COW_BITS 12
#define GRUB_DISK_COW_SIZE (1U << GRUB_DISK_COW_BITS)
#define GRUB_DISK_COW_BUCKETS 64

struct grub_disk_cow_block
{
  struct grub_disk_cow_block *next;
  grub_uint64_t offset;
  grub_uint8_t data[GRUB_DISK_COW_SIZE];
};

struct grub_disk_overlay
{
  struct grub_disk_cow_block *buckets[GRUB_DISK_COW_BUCKETS];
};

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

      if (stage == GRUB_DISK_FORMAT
          && !(ctx->flags & GRUB_DISK_PIPELINE_FORMAT))
        continue;

      for (hook = grub_disk_hooks; hook; hook = hook->next)
        {
          grub_err_t err;

          if (hook->stage != stage)
            continue;
          if (hook->probe && !hook->probe (ctx, hook))
            {
              /* Probes are observational. A mismatch is not a failure. */
              grub_errno = GRUB_ERR_NONE;
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

static struct grub_disk_cow_block *
grub_disk_cow_find (struct grub_disk_overlay *overlay, grub_uint64_t offset)
{
  struct grub_disk_cow_block *block;
  unsigned int bucket = (offset >> GRUB_DISK_COW_BITS) % GRUB_DISK_COW_BUCKETS;

  for (block = overlay->buckets[bucket]; block; block = block->next)
    if (block->offset == offset)
      return block;
  return 0;
}

grub_err_t
grub_disk_pipeline_enable_cow (struct grub_disk_pipeline *ctx)
{
  if (!ctx || !ctx->file)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "no disk to overlay");
  if (!ctx->overlay)
    {
      ctx->overlay = grub_zalloc (sizeof (*ctx->overlay));
      if (!ctx->overlay)
        return grub_errno;
    }
  return GRUB_ERR_NONE;
}

grub_err_t
grub_disk_pipeline_read (struct grub_disk_pipeline *ctx,
                         grub_uint64_t off, grub_size_t len, void *buf)
{
  grub_uint64_t cur = off;
  grub_size_t done = 0;
  grub_err_t err;

  if (!ctx || !buf)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid disk read");
  if (!len)
    return GRUB_ERR_NONE;

  err = grub_disk_pipeline_read_base (ctx, off, len, buf);
  if (err || !ctx->overlay)
    return err;

  while (done < len)
    {
      grub_uint64_t start = cur & ~((grub_uint64_t) GRUB_DISK_COW_SIZE - 1);
      grub_size_t inside = cur - start;
      grub_size_t size = GRUB_DISK_COW_SIZE - inside;
      struct grub_disk_cow_block *block;

      if (size > len - done)
        size = len - done;
      block = grub_disk_cow_find (ctx->overlay, start);
      if (block)
        grub_memcpy ((grub_uint8_t *) buf + done, block->data + inside, size);
      cur += size;
      done += size;
    }
  return GRUB_ERR_NONE;
}

grub_err_t
grub_disk_pipeline_write (struct grub_disk_pipeline *ctx,
                          grub_uint64_t off, grub_size_t len, const void *buf)
{
  grub_size_t done = 0;

  if (!ctx || !ctx->file || !buf)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid disk write");
  if (!len)
    return GRUB_ERR_NONE;
  if (ctx->file->size != GRUB_FILE_SIZE_UNKNOWN
      && (off > ctx->file->size || len > ctx->file->size - off))
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "disk write outside image");

  if (!ctx->overlay)
    return ctx->direct_write
      ? ctx->direct_write (ctx->file, buf, len, off)
      : grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET, "disk is read-only");

  while (done < len)
    {
      grub_uint64_t cur = off + done;
      grub_uint64_t start = cur & ~((grub_uint64_t) GRUB_DISK_COW_SIZE - 1);
      unsigned int bucket = (start >> GRUB_DISK_COW_BITS) % GRUB_DISK_COW_BUCKETS;
      grub_size_t inside = cur - start;
      grub_size_t size = GRUB_DISK_COW_SIZE - inside;
      struct grub_disk_cow_block *block = grub_disk_cow_find (ctx->overlay, start);

      if (size > len - done)
        size = len - done;
      if (!block)
        {
          grub_err_t err;
          block = grub_malloc (sizeof (*block));
          if (!block)
            return grub_errno;
          err = grub_disk_pipeline_read_base (ctx, start, GRUB_DISK_COW_SIZE,
                                              block->data);
          if (err)
            {
              grub_free (block);
              return err;
            }
          block->offset = start;
          block->next = ctx->overlay->buckets[bucket];
          ctx->overlay->buckets[bucket] = block;
        }
      grub_memcpy (block->data + inside, (const grub_uint8_t *) buf + done, size);
      done += size;
    }
  return GRUB_ERR_NONE;
}

void
grub_disk_pipeline_close (struct grub_disk_pipeline *ctx)
{
  if (!ctx)
    return;

  if (ctx->overlay)
    {
      unsigned int i;
      for (i = 0; i < GRUB_DISK_COW_BUCKETS; i++)
        {
          struct grub_disk_cow_block *block = ctx->overlay->buckets[i];
          while (block)
            {
              struct grub_disk_cow_block *next = block->next;
              grub_free (block);
              block = next;
            }
        }
      grub_free (ctx->overlay);
    }

  if (ctx->file)
    grub_file_close (ctx->file);
  if (ctx->format_module)
    grub_dl_unref (ctx->format_module);
  grub_free (ctx->source);
  grub_free (ctx->name);
  grub_free (ctx);
}
