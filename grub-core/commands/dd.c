/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2019  Free Software Foundation, Inc.
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
/*
 *  BURG - Brand-new Universal loadeR from GRUB
 *  Copyright 2009 Bean Lee - All Rights Reserved
 *
 *  BURG is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include <grub/dl.h>
#include <grub/mm.h>
#include <grub/file.h>
#include <grub/misc.h>
#include <grub/partition.h>
#include <grub/msdos_partition.h>
#include <grub/gpt_partition.h>
#include <grub/fs.h>
#include <grub/extcmd.h>
#include <grub/normal.h>
#include <grub/i18n.h>
#include <grub/bind.h>
#include <grub/port_write.h>

GRUB_MOD_LICENSE ("GPLv3+");

static const struct grub_arg_option options[] =
{
  {"if", 'i', 0, N_("Specify input file."), "FILE", ARG_TYPE_STRING},
  {"str", 's', 0, N_("Specify input string."), "STRING", ARG_TYPE_STRING},
  {"hex", 'x', 0, N_("Specify input hex string."), "HEX", ARG_TYPE_STRING},
  {"of", 'o', 0, N_("Specify output file."), "FILE", ARG_TYPE_STRING},
  {"bs", 'b', 0, N_("Specify block size (1~4096)."), "BYTES", ARG_TYPE_INT},
  {"count", 'c', 0, N_("Number of blocks to copy."), "BLOCKS", ARG_TYPE_INT},
  {"skip", 'k', 0, N_("Skip N bytes at input."), "BYTES", ARG_TYPE_INT},
  {"seek", 'e', 0, N_("Skip N bytes at output."), "BYTES", ARG_TYPE_INT},
  {0, 0, 0, 0, 0, 0}
};

enum options
{
  DD_IF,
  DD_STR,
  DD_HEX,
  DD_OF,
  DD_BS,
  DD_COUNT,
  DD_SKIP,
  DD_SEEK,
};

static grub_err_t
grub_cmd_dd (grub_extcmd_context_t ctxt, int argc __attribute__ ((unused)),
             char **args __attribute__ ((unused)))
{
  struct grub_arg_list *state = ctxt->state;
  grub_uint8_t data[4096];
  /* input */
  char *str = NULL;
  char *hexstr = NULL;
  grub_file_t in_file = NULL;
  grub_off_t in_size = 0;
  /* output */
  grub_file_t out_file = NULL;
  grub_off_t out_size = 0;
  /* block size */
  grub_uint32_t bs = 1;
  /* skip & seek */
  grub_off_t skip = 0;
  grub_off_t seek = 0;
  /* count */
  grub_off_t count = 0;

  if (state[DD_IF].set)
  {
    in_file = grub_file_open (state[DD_IF].arg,
                              GRUB_FILE_TYPE_HEXCAT |
                              GRUB_FILE_TYPE_NO_DECOMPRESS);
    if (! in_file)
      return grub_error (GRUB_ERR_BAD_FILENAME, N_("failed to open %s"),
                         state[DD_IF].arg);
    in_size = grub_file_size (in_file);
  }

  if (state[DD_STR].set)
  {
    str = state[DD_STR].arg;
    in_size = grub_strlen (str);
  }

  if (state[DD_HEX].set)
  {
    int i, size;
    str = state[DD_HEX].arg;
    size = grub_strlen (str) / 2;
    if (size == 0)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid hex string");

    hexstr = grub_zalloc (size);
    if (! hexstr)
      return grub_errno;

    for (i = 0; i < size * 2; i++)
    {
      int c;
      if ((str[i] >= '0') && (str[i] <= '9'))
        c = str[i] - '0';
      else if ((str[i] >= 'A') && (str[i] <= 'F'))
        c = str[i] - 'A' + 10;
      else if ((str[i] >= 'a') && (str[i] <= 'f'))
        c = str[i] - 'a' + 10;
      else
      {
        grub_free (hexstr);
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid hex string");
      }
      if ((i & 1) == 0)
        c <<= 4;
      hexstr[i >> 1] |= c;
    }
    str = hexstr;
    in_size = size;
  }

  if (state[DD_OF].set)
  {
    out_file = grub_file_open (state[DD_OF].arg,
                               GRUB_FILE_TYPE_HEXCAT |
                               GRUB_FILE_TYPE_NO_DECOMPRESS);
    if (! out_file)
      return grub_error (GRUB_ERR_BAD_FILENAME, N_("failed to open %s"),
                         state[DD_OF].arg);
    out_size = grub_file_size (out_file);
    if (!grub_port_file_prepare_write (out_file))
      return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                         N_("dd write supports mem: or disk-backed files only"));
  }

  if (((! in_file) && (! str)) || (! out_file))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "no input or output file");

  if (state[DD_BS].set)
  {
    bs = grub_strtoul (state[DD_BS].arg, 0, 0);
    if (! bs || bs > 4096)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid block size");
  }

  if (state[DD_COUNT].set)
  {
    count = grub_strtoull (state[DD_COUNT].arg, 0, 0);
    if (! count)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid count");
  }

  if (state[DD_SKIP].set)
    skip = grub_strtoull (state[DD_SKIP].arg, 0, 0);

  if (state[DD_SEEK].set)
    seek = grub_strtoull (state[DD_SEEK].arg, 0, 0);

  count *= bs;

  if ((skip >= in_size) || (seek >= out_size))
  {
    grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid skip/seek");
    goto fail;
  }

  if (!count)
    count = in_size - skip;

  if (skip + count > in_size)
  {
    grub_printf ("WARNING: skip + count > input_size\n");
    count = in_size - skip;
  }

  if (seek + count > out_size)
  {
    grub_printf ("WARNING: seek + count > output_size\n");
    count = out_size - seek;
  }

  while (count)
  {
    grub_uint32_t copy_bs;
    copy_bs = (bs > count) ? count : bs;
    /* read */
    if (in_file)
    {
      grub_file_seek (in_file, skip);
      grub_file_read (in_file, data, copy_bs);
      if (grub_errno)
        break;
    }
    else
    {
      grub_memcpy (data, str + skip, copy_bs);
    }
    /* write */
    grub_file_seek (out_file, seek);
    if (grub_port_file_write (out_file, (const char *) data, copy_bs) < 0)
      break;

    skip += copy_bs;
    seek += copy_bs;
    count -= copy_bs;
  }

fail:
  if (hexstr)
    grub_free (hexstr);
  if (in_file)
    grub_file_close (in_file);
  if (out_file)
    grub_file_close (out_file);
  return grub_errno;
}

static grub_extcmd_t cmd;

/* Native disk I/O is deliberately independent of WriteFS.  This is an
   explicit raw block-device API, not a file-system write entry point. */
static grub_dl_t disk_module;

static void
disk_handle_destroy (void *data)
{
  grub_disk_close (data);
}

static grub_disk_t
disk_from_value (const struct grub_bind_value *value)
{
  return grub_bind_handle_data (value->value.handle, "disk");
}

static void
disk_bind_string (struct grub_bind_value *result, const char *value, int owned)
{
  result->type = GRUB_BIND_STRING;
  result->owned = owned;
  result->value.bytes.data = value ? value : "";
  result->value.bytes.size = grub_strlen (value ? value : "");
}

static grub_err_t
bind_disk_open (grub_size_t argc __attribute__ ((unused)),
                const struct grub_bind_value *args,
                struct grub_bind_value *results, grub_size_t *nresults)
{
  const char *name = args[0].value.bytes.data;
  grub_disk_t disk;
  struct grub_bind_handle *handle;
  char *bare = NULL;
  grub_size_t len = args[0].value.bytes.size;

  results[0].type = GRUB_BIND_NIL;
  *nresults = 1;
  if (len >= 2 && name[0] == '(' && name[len - 1] == ')')
    {
      bare = grub_strndup (name + 1, len - 2);
      if (!bare) return grub_errno;
      name = bare;
    }
  disk = grub_disk_open (name);
  grub_free (bare);
  if (!disk)
    {
      /* Legacy disk.open returns nil for disks that cannot be opened. */
      grub_errno = GRUB_ERR_NONE;
      return GRUB_ERR_NONE;
    }
  handle = grub_bind_handle_create (disk_module, "disk", disk,
                                   disk_handle_destroy);
  if (!handle)
    {
      grub_disk_close (disk);
      return grub_errno;
    }
  results[0].type = GRUB_BIND_HANDLE;
  results[0].value.handle = handle;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_disk_close (grub_size_t argc __attribute__ ((unused)),
                 const struct grub_bind_value *args,
                 struct grub_bind_value *results __attribute__ ((unused)),
                 grub_size_t *nresults)
{
  *nresults = 0;
  if (!disk_from_value (&args[0])) return grub_errno;
  return grub_bind_handle_close (args[0].value.handle);
}

static grub_err_t
bind_disk_read (grub_size_t argc __attribute__ ((unused)),
                const struct grub_bind_value *args,
                struct grub_bind_value *results, grub_size_t *nresults)
{
  grub_disk_t disk = disk_from_value (&args[0]);
  grub_uint64_t length = args[3].value.u64;
  grub_uint64_t offset = args[2].value.u64;
  void *buffer;
  grub_err_t err;
  if (!disk) return grub_errno;
  if ((grub_size_t) length != length || offset > 0x7fffffffffffffffULL)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "raw disk read range too large");
  if (!length)
    buffer = NULL;
  else
    {
      buffer = grub_malloc ((grub_size_t) length);
      if (!buffer) return grub_errno;
      err = grub_disk_read (disk, args[1].value.u64, offset, length, buffer);
      if (err)
        {
          grub_free (buffer);
          return err;
        }
    }
  results[0].type = GRUB_BIND_BYTES;
  results[0].owned = buffer != NULL;
  results[0].value.bytes.data = buffer ? buffer : "";
  results[0].value.bytes.size = length;
  *nresults = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_disk_write (grub_size_t argc __attribute__ ((unused)),
                 const struct grub_bind_value *args,
                 struct grub_bind_value *results __attribute__ ((unused)),
                 grub_size_t *nresults)
{
  grub_disk_t disk = disk_from_value (&args[0]);
  grub_uint64_t length = args[3].value.u64;
  grub_uint64_t offset = args[2].value.u64;
  if (!disk) return grub_errno;
  if (length > args[4].value.bytes.size ||
      (grub_size_t) length != length || offset > 0x7fffffffffffffffULL)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid raw disk write size");
  *nresults = 0;
  if (!length) return GRUB_ERR_NONE;
  return grub_disk_write (disk, args[1].value.u64, offset, length,
                          args[4].value.bytes.data);
}

enum disk_query
{
  DISK_PARTMAP, DISK_DRIVER, DISK_FS, DISK_UUID, DISK_LABEL, DISK_SIZE,
  DISK_BOOTABLE
};

static grub_err_t
bind_disk_query (const struct grub_bind_value *args,
                 struct grub_bind_value *results, grub_size_t *nresults,
                 enum disk_query query, int human)
{
  grub_disk_t disk = disk_from_value (&args[0]);
  struct grub_device dev = { NULL, NULL };
  grub_fs_t fs;
  char *value = NULL;
  const char *text = "";
  int owned = 0, bootable = 0;
  if (!disk) return grub_errno;
  dev.disk = disk;
  switch (query)
    {
    case DISK_PARTMAP:
      text = disk->partition && disk->partition->partmap ?
        disk->partition->partmap->name : "none";
      break;
    case DISK_DRIVER:
      text = disk->dev ? disk->dev->name : "none";
      break;
    case DISK_FS:
    case DISK_UUID:
    case DISK_LABEL:
      fs = grub_fs_probe (&dev);
      if (query == DISK_FS)
        text = fs ? fs->name : "none";
      else if (fs)
        {
          if (query == DISK_UUID && fs->fs_uuid)
            fs->fs_uuid (&dev, &value);
          if (query == DISK_LABEL && fs->fs_label)
            fs->fs_label (&dev, &value);
          text = value ? value : "";
          owned = value != NULL;
        }
      /* A disk without a recognized filesystem is not a script error. */
      grub_errno = GRUB_ERR_NONE;
      break;
    case DISK_SIZE:
      {
        grub_uint64_t size = grub_disk_native_sectors (disk) <<
                             GRUB_DISK_SECTOR_BITS;
        char buf[40];
        const char *formatted;
        if (human)
          formatted = grub_get_human_size (size, GRUB_HUMAN_SIZE_SHORT);
        else
          {
            grub_snprintf (buf, sizeof (buf), "%llu",
                           (unsigned long long) size);
            formatted = buf;
          }
        value = grub_strdup (formatted);
        if (!value) return grub_errno;
        text = value;
        owned = 1;
      }
      break;
    case DISK_BOOTABLE:
      {
        grub_partition_t p = disk->partition;
        if (p && p->partmap && !grub_strcmp (p->partmap->name, "msdos"))
          {
            struct grub_msdos_partition_mbr mbr;
            if (p->index >= 0 && p->index < 4 &&
                !grub_disk_read (disk, p->offset, 0, sizeof (mbr), &mbr))
              bootable = !!(mbr.entries[p->index].flag & 0x80);
            grub_errno = GRUB_ERR_NONE;
          }
        else if (p && p->partmap && !grub_strcmp (p->partmap->name, "gpt"))
          {
            struct grub_gpt_partentry entry;
            grub_packed_guid_t efi_guid = GRUB_GPT_PARTITION_TYPE_EFI_SYSTEM;
            if (!grub_disk_read (disk, p->offset, p->index,
                                 sizeof (entry), &entry) &&
                !grub_memcmp (&entry.type, &efi_guid, sizeof (efi_guid)))
              bootable = 1;
            grub_errno = GRUB_ERR_NONE;
          }
        results[0].type = GRUB_BIND_BOOL;
        results[0].value.boolean = bootable;
        *nresults = 1;
        return GRUB_ERR_NONE;
      }
    }
  disk_bind_string (&results[0], text, owned);
  *nresults = 1;
  return GRUB_ERR_NONE;
}

#define DISK_QUERY_CALLBACK(name, kind) \
  static grub_err_t \
  bind_disk_##name (grub_size_t argc __attribute__ ((unused)), \
                    const struct grub_bind_value *args, \
                    struct grub_bind_value *results, grub_size_t *nresults) \
  { return bind_disk_query (args, results, nresults, kind, 0); }

DISK_QUERY_CALLBACK(partmap, DISK_PARTMAP)
DISK_QUERY_CALLBACK(driver, DISK_DRIVER)
DISK_QUERY_CALLBACK(fs, DISK_FS)
DISK_QUERY_CALLBACK(fsuuid, DISK_UUID)
DISK_QUERY_CALLBACK(label, DISK_LABEL)
DISK_QUERY_CALLBACK(bootable, DISK_BOOTABLE)

static grub_err_t
bind_disk_size (grub_size_t argc, const struct grub_bind_value *args,
                struct grub_bind_value *results, grub_size_t *nresults)
{
  return bind_disk_query (args, results, nresults, DISK_SIZE,
                          argc > 1);
}

static const enum grub_bind_type disk_open_args[] = { GRUB_BIND_STRING };
static const enum grub_bind_type disk_handle_args[] = { GRUB_BIND_HANDLE };
static const enum grub_bind_type disk_read_args[] =
  { GRUB_BIND_HANDLE, GRUB_BIND_U64, GRUB_BIND_U64, GRUB_BIND_U64 };
static const enum grub_bind_type disk_write_args[] =
  { GRUB_BIND_HANDLE, GRUB_BIND_U64, GRUB_BIND_U64, GRUB_BIND_U64,
    GRUB_BIND_BYTES };
static const enum grub_bind_type disk_size_args[] =
  { GRUB_BIND_HANDLE, GRUB_BIND_BOOL };
static const struct grub_bind_export disk_exports[] =
{
  { "disk", "open",     disk_open_args,   1, 1, bind_disk_open },
  { "disk", "close",    disk_handle_args, 1, 1, bind_disk_close },
  { "disk", "read",     disk_read_args,   4, 4, bind_disk_read },
  { "disk", "write",    disk_write_args,  5, 5, bind_disk_write },
  { "disk", "partmap",  disk_handle_args, 1, 1, bind_disk_partmap },
  { "disk", "driver",   disk_handle_args, 1, 1, bind_disk_driver },
  { "disk", "fs",       disk_handle_args, 1, 1, bind_disk_fs },
  { "disk", "fsuuid",   disk_handle_args, 1, 1, bind_disk_fsuuid },
  { "disk", "label",    disk_handle_args, 1, 1, bind_disk_label },
  { "disk", "size",     disk_size_args,   1, 2, bind_disk_size },
  { "disk", "bootable", disk_handle_args, 1, 1, bind_disk_bootable }
};

GRUB_MOD_INIT(dd)
{
  cmd = grub_register_extcmd ("dd", grub_cmd_dd, 0,
                              N_("[OPTIONS]"), N_("Copy data."), options);
  disk_module = mod;
  grub_bind_register (mod, disk_exports, 11);
}

GRUB_MOD_FINI(dd)
{
  grub_bind_unregister (disk_module);
  disk_module = NULL;
  grub_unregister_extcmd (cmd);
}
