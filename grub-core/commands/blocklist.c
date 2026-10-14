/* blocklist.c - print the block list of a file */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2006,2007  Free Software Foundation, Inc.
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

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/disk.h>
#include <grub/partition.h>
#include <grub/command.h>
#include <grub/extcmd.h>
#include <grub/env.h>
#include <grub/i18n.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* Context for grub_cmd_blocklist.  */
struct blocklist_ctx
{
  grub_disk_addr_t start_sector;
  unsigned num_sectors;
  int num_entries;
  grub_disk_addr_t part_start;
  char *text;
  grub_size_t length, capacity;
};

static void
append_text (struct blocklist_ctx *ctx, const char *s)
{
  grub_size_t n = grub_strlen (s), need = ctx->length + n + 1;
  char *p;
  if (need < ctx->length)
    { grub_error (GRUB_ERR_OUT_OF_RANGE, N_("block list is too large")); return; }
  if (need > ctx->capacity)
    {
      grub_size_t cap = ctx->capacity ? ctx->capacity : 64;
      while (cap < need && cap <= (grub_size_t) -1 / 2) cap *= 2;
      if (cap < need) cap = need;
      p = grub_realloc (ctx->text, cap);
      if (!p) return;
      ctx->text = p; ctx->capacity = cap;
    }
  grub_memcpy (ctx->text + ctx->length, s, n + 1);
  ctx->length += n;
}

/* Helper for grub_cmd_blocklist.  */
static void
print_blocklist (grub_disk_addr_t sector, unsigned num,
		 unsigned offset, unsigned length, struct blocklist_ctx *ctx)
{
  char entry[96];
  if (ctx->num_entries++) append_text (ctx, ",");
  grub_snprintf (entry, sizeof (entry), "%llu",
                 (unsigned long long) (sector - ctx->part_start));
  append_text (ctx, entry);
  if (num > 0)
    { grub_snprintf (entry, sizeof (entry), "+%u", num); append_text (ctx, entry); }
  if (offset != 0 || length != 0)
    { grub_snprintf (entry, sizeof (entry), "[%u-%u]", offset, offset + length); append_text (ctx, entry); }
}

/* Helper for grub_cmd_blocklist.  */
static grub_err_t
read_blocklist (grub_disk_addr_t sector, unsigned offset, unsigned length,
		char *buf __attribute__ ((unused)), void *data)
{
  struct blocklist_ctx *ctx = data;

  if (ctx->num_sectors > 0)
    {
      if (ctx->start_sector + ctx->num_sectors == sector
	  && offset == 0 && length >= GRUB_DISK_SECTOR_SIZE)
	{
	  ctx->num_sectors += length >> GRUB_DISK_SECTOR_BITS;
	  sector += length >> GRUB_DISK_SECTOR_BITS;
	  length &= (GRUB_DISK_SECTOR_SIZE - 1);
	}

      if (!length)
	return GRUB_ERR_NONE;
      print_blocklist (ctx->start_sector, ctx->num_sectors, 0, 0, ctx);
      ctx->num_sectors = 0;
    }

  if (offset)
    {
      unsigned l = length + offset;
      l &= (GRUB_DISK_SECTOR_SIZE - 1);
      l -= offset;
      print_blocklist (sector, 0, offset, l, ctx);
      length -= l;
      sector++;
      offset = 0;
    }

  if (!length)
    return GRUB_ERR_NONE;

  if (length & (GRUB_DISK_SECTOR_SIZE - 1))
    {
      if (length >> GRUB_DISK_SECTOR_BITS)
	{
	  print_blocklist (sector, length >> GRUB_DISK_SECTOR_BITS, 0, 0, ctx);
	  sector += length >> GRUB_DISK_SECTOR_BITS;
	}
      print_blocklist (sector, 0, 0, length & (GRUB_DISK_SECTOR_SIZE - 1), ctx);
    }
  else
    {
      ctx->start_sector = sector;
      ctx->num_sectors = length >> GRUB_DISK_SECTOR_BITS;
    }

  return GRUB_ERR_NONE;
}

static const struct grub_arg_option options[] =
  {
    {"set", 's', 0, N_("Set a variable to return value."), "VAR", ARG_TYPE_STRING},
    {"offset", 'o', 0, N_("Set file offset."), "BYTES", ARG_TYPE_INT},
    {"length", 'l', 0, N_("Set blocklist length."), "BYTES", ARG_TYPE_INT},
    {0, 0, 0, 0, 0, 0}
  };

static grub_err_t
grub_cmd_blocklist (grub_extcmd_context_t ctxt, int argc, char **args)
{
  grub_file_t file;
  char buf[GRUB_DISK_SECTOR_SIZE];
  struct blocklist_ctx ctx = {
    .start_sector = 0,
    .num_sectors = 0,
    .num_entries = 0,
    .part_start = 0,
    .text = 0, .length = 0, .capacity = 0
  };

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  file = grub_file_open (args[0], GRUB_FILE_TYPE_PRINT_BLOCKLIST
			 | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (! file)
    return grub_errno;

  if (! file->device->disk)
    { grub_error (GRUB_ERR_BAD_DEVICE, "this command is available only for disk devices"); goto done; }

  ctx.part_start = grub_partition_get_start (file->device->disk->partition);
  if (ctxt->state[1].set)
    file->offset = grub_strtoull (ctxt->state[1].arg, 0, 0);

  file->read_hook = read_blocklist;
  file->read_hook_data = &ctx;

  if (ctxt->state[2].set)
    {
      grub_off_t limit = grub_strtoull (ctxt->state[2].arg, 0, 0);
      while (limit > 0)
        {
          grub_ssize_t n = grub_file_read (file, buf,
                          limit < sizeof (buf) ? limit : sizeof (buf));
          if (n <= 0) break;
          limit -= n;
        }
    }
  else
    while (grub_file_read (file, buf, sizeof (buf)) > 0)
      ;

  if (ctx.num_sectors > 0)
    print_blocklist (ctx.start_sector, ctx.num_sectors, 0, 0, &ctx);
  if (ctxt->state[0].set)
    grub_env_set (ctxt->state[0].arg, ctx.text ? ctx.text : "");
  else
    grub_printf ("%s\n", ctx.text ? ctx.text : "");

done:
  grub_free (ctx.text);
  grub_file_close (file);

  return grub_errno;
}

static grub_extcmd_t cmd;

GRUB_MOD_INIT(blocklist)
{
  cmd = grub_register_extcmd ("blocklist", grub_cmd_blocklist, 0,
                              N_("[OPTIONS] FILE"), N_("Print a block list."), options);
}

GRUB_MOD_FINI(blocklist)
{
  grub_unregister_extcmd (cmd);
}
