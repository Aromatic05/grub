/* fileview.c - shared helpers for transformed file views.  */
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

#include <grub/file.h>
#include <grub/fileview.h>
#include <grub/disk_pipeline.h>
#include <grub/misc.h>

/* The slots only track registration identities. The ordered list and
   dispatch both belong to the common disk Stage registry. */
struct grub_fileview_slot
{
  struct grub_disk_hook hook;
  grub_fileview_transform_t transform;
};

static struct grub_fileview_slot grub_fileview_slots[GRUB_FILE_FILTER_MAX];

static grub_err_t
grub_fileview_run (struct grub_disk_pipeline *ctx,
                   const struct grub_disk_hook *hook)
{
  struct grub_fileview_slot *slot = (void *) hook->data;
  grub_file_t next = slot->transform (ctx->file, ctx->type);

  if (!next)
    return grub_errno ? grub_errno
                      : grub_error (GRUB_ERR_BAD_COMPRESSED_DATA,
                                    "disk decompression failed");
  ctx->file = next;
  return GRUB_ERR_NONE;
}

void
EXPORT_FUNC (grub_fileview_register) (grub_file_filter_id_t id,
                                      const char *name,
                                      grub_fileview_transform_t transform)
{
  struct grub_fileview_slot *slot;

  if (id < GRUB_FILE_FILTER_COMPRESSION_FIRST
      || id > GRUB_FILE_FILTER_COMPRESSION_LAST || !transform)
    return;
  slot = &grub_fileview_slots[id];
  slot->transform = transform;
  slot->hook.name = name ? name : "unknown";
  slot->hook.stage = GRUB_DISK_VIEW;
  slot->hook.order = id;
  slot->hook.run = grub_fileview_run;
  slot->hook.data = slot;
  grub_disk_hook_register (&slot->hook, 0);
}

void
EXPORT_FUNC (grub_fileview_unregister) (grub_file_filter_id_t id)
{
  if (id < GRUB_FILE_FILTER_COMPRESSION_FIRST
      || id > GRUB_FILE_FILTER_COMPRESSION_LAST)
    return;
  grub_disk_hook_unregister (&grub_fileview_slots[id].hook);
  grub_fileview_slots[id].transform = 0;
}

void
EXPORT_FUNC (grub_fileview_register_many) (const struct grub_fileview_desc *views,
                                           grub_size_t count)
{
  grub_size_t i;
  for (i = 0; i < count; i++)
    grub_fileview_register (views[i].id, views[i].name, views[i].transform);
}

void
EXPORT_FUNC (grub_fileview_unregister_many) (const struct grub_fileview_desc *views,
                                             grub_size_t count)
{
  grub_size_t i;
  for (i = 0; i < count; i++)
    grub_fileview_unregister (views[i].id);
}

const char *
EXPORT_FUNC (grub_fileview_name) (grub_file_filter_id_t id)
{
  if (id < GRUB_FILE_FILTER_COMPRESSION_FIRST
      || id > GRUB_FILE_FILTER_COMPRESSION_LAST)
    return 0;
  return grub_fileview_slots[id].hook.name;
}

grub_file_t
EXPORT_FUNC (grub_fileview_apply_compression) (grub_file_t file,
                                               enum grub_file_type type)
{
  struct grub_disk_pipeline ctx;

  if (!file || !grub_fileview_allow_decompress (type))
    return file;
  grub_memset (&ctx, 0, sizeof (ctx));
  ctx.file = file;
  ctx.type = type;
  ctx.flags = GRUB_DISK_PIPELINE_DECOMPRESS;
  if (grub_disk_pipeline_views (&ctx))
    {
      /* A failed decoder never consumes the original view. */
      grub_file_close (ctx.file);
      return 0;
    }
  return ctx.file;
}
