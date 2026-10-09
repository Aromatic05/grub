/* Sparse volatile copy-on-write BLOCK hook for the disk pipeline. */
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

#define COW_BITS 12
#define COW_SIZE (1U << COW_BITS)
#define COW_BUCKETS 64
#define COW_MAX_BLOCKS (256U * 1024U * 1024U / COW_SIZE)

struct cow_block
{
  struct cow_block *next;
  grub_uint64_t offset;
  grub_uint8_t data[COW_SIZE];
};

struct cow_layer
{
  struct grub_disk_layer io;
  struct cow_block *buckets[COW_BUCKETS];
  grub_size_t blocks;
};

static struct cow_block *
cow_lookup (struct cow_layer *cow, grub_uint64_t start)
{
  struct cow_block *block;
  unsigned int slot = (start >> COW_BITS) % COW_BUCKETS;
  for (block = cow->buckets[slot]; block; block = block->next)
    if (block->offset == start)
      return block;
  return 0;
}

static grub_err_t
cow_read (struct grub_disk_layer *io, grub_uint64_t off,
          grub_size_t len, void *buf)
{
  struct cow_layer *cow = io->data;
  grub_size_t done = 0;
  while (done < len)
    {
      grub_uint64_t pos = off + done;
      grub_uint64_t start = pos & ~((grub_uint64_t) COW_SIZE - 1);
      grub_size_t inside = pos - start;
      grub_size_t n = COW_SIZE - inside;
      struct cow_block *block = cow_lookup (cow, start);
      if (n > len - done)
        n = len - done;
      if (block)
        grub_memcpy ((grub_uint8_t *) buf + done, block->data + inside, n);
      else
        {
          grub_err_t err = io->lower->read (io->lower, pos, n,
                                            (grub_uint8_t *) buf + done);
          if (err)
            return err;
        }
      done += n;
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
cow_write (struct grub_disk_layer *io, grub_uint64_t off,
           grub_size_t len, const void *buf)
{
  struct cow_layer *cow = io->data;
  grub_size_t done = 0;
  while (done < len)
    {
      grub_uint64_t pos = off + done;
      grub_uint64_t start = pos & ~((grub_uint64_t) COW_SIZE - 1);
      grub_size_t inside = pos - start;
      grub_size_t n = COW_SIZE - inside;
      struct cow_block *block = cow_lookup (cow, start);
      if (n > len - done)
        n = len - done;
      if (!block)
        {
          unsigned int slot = (start >> COW_BITS) % COW_BUCKETS;
          if (cow->blocks >= COW_MAX_BLOCKS)
            return grub_error (GRUB_ERR_OUT_OF_MEMORY,
                               "copy-on-write overlay memory limit reached");
          block = grub_malloc (sizeof (*block));
          if (!block)
            return grub_errno;
          /* Entirely overwritten blocks need no backing read. */
          if (inside == 0 && n == COW_SIZE)
            grub_memset (block->data, 0, COW_SIZE);
          else
            {
              grub_err_t err = io->lower->read (io->lower, start, COW_SIZE,
                                                block->data);
              if (err)
                {
                  grub_free (block);
                  return err;
                }
            }
          block->offset = start;
          block->next = cow->buckets[slot];
          cow->buckets[slot] = block;
          cow->blocks++;
        }
      grub_memcpy (block->data + inside, (const grub_uint8_t *) buf + done, n);
      done += n;
    }
  return GRUB_ERR_NONE;
}

static void
cow_close (struct grub_disk_layer *io)
{
  struct cow_layer *cow = io->data;
  unsigned int i;
  for (i = 0; i < COW_BUCKETS; i++)
    {
      struct cow_block *block = cow->buckets[i];
      while (block)
        {
          struct cow_block *next = block->next;
          grub_free (block);
          block = next;
        }
    }
  grub_free (cow);
}

static int
cow_probe (struct grub_disk_pipeline *ctx,
           const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  return !!((ctx->flags & GRUB_DISK_PIPELINE_COW) || ctx->transformed);
}

static grub_err_t
cow_install (struct grub_disk_pipeline *ctx,
             const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  struct cow_layer *cow;
  if (!ctx->io)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "copy-on-write without base block reader");
  cow = grub_zalloc (sizeof (*cow));
  if (!cow)
    return grub_errno;
  cow->io.data = cow;
  cow->io.lower = ctx->io;
  cow->io.read = cow_read;
  cow->io.write = cow_write;
  cow->io.close = cow_close;
  ctx->io = &cow->io;
  return GRUB_ERR_NONE;
}

void
grub_disk_overlay_register (void)
{
  static struct grub_disk_hook hook =
    { .name = "cow", .stage = GRUB_DISK_BLOCK,
      .probe = cow_probe, .run = cow_install };
  grub_disk_hook_register (&hook, 0);
}
