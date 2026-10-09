 /*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2019,2020  Free Software Foundation, Inc.
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
 *
 */

#include <grub/dl.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/efi/disk.h>
#include <grub/device.h>
#include <grub/err.h>
#include <grub/env.h>
#include <grub/extcmd.h>
#include <grub/file.h>
#include <grub/i18n.h>
#include <grub/loader.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/term.h>

#include <iso.h>
#include <guid.h>
#include <misc.h>
#include <grub4dos.h>

GRUB_MOD_LICENSE ("GPLv3+");

void grub_ventoy_set_osparam (const char *filename);
void grub_ventoy_set_acpi_osparam (const char *filename);

static unsigned int last_id = 0;
static grub_efi_handle_t boot_image_handle = 0;

static void *
grub_map_source_allocate (struct grub_disk_pipeline *ctx, grub_size_t size)
{
  return grub_map_allocate_pages
    (size, (ctx->flags & GRUB_DISK_PIPELINE_RESERVED_MEMORY)
            ? GRUB_EFI_RESERVED_MEMORY_TYPE : GRUB_EFI_BOOT_SERVICES_DATA);
}

static void
grub_map_source_release (struct grub_disk_pipeline *ctx __attribute__ ((unused)),
                         void *addr, grub_size_t size)
{
  grub_efi_boot_services_t *b = grub_efi_system_table->boot_services;
  grub_efi_uintn_t pages = (size + 4095) >> 12;
  efi_call_2 (b->free_pages, (grub_efi_physical_address_t) (grub_addr_t) addr, pages);
}

const grub_efi_guid_t dp_guid = GRUB_EFI_DEVICE_PATH_GUID;
const grub_efi_guid_t blk_io_guid = GRUB_EFI_BLOCK_IO_GUID;

static grub_err_t
grub_efiloader_unload (void)
{
  grub_efi_boot_services_t *b = grub_efi_system_table->boot_services;
  efi_call_1 (b->unload_image, boot_image_handle);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_efiloader_boot (void)
{
  grub_efi_boot_services_t *b = grub_efi_system_table->boot_services;
  grub_script_execute_sourcecode ("terminal_output console");
  grub_printf ("Booting from vdisk ...\n");
  grub_refresh ();
  efi_call_3 (b->start_image, boot_image_handle, 0, NULL);

  grub_loader_unset ();

  return grub_errno;
}

static void
unmap_efidisk (grub_disk_t disk)
{
  grub_efi_handle_t handle;
  grub_efi_device_path_t *device_path = NULL;
  grub_efi_block_io_t *block_io = NULL;
  grub_efi_boot_services_t *b = grub_efi_system_table->boot_services;
  grub_efi_status_t status;

  if (!disk)
    return;

  handle = grub_efidisk_get_device_handle (disk);
  if (!handle)
    {
      grub_error (GRUB_ERR_BAD_DEVICE, "failed to resolve efidisk handle");
      return;
    }

  status = efi_call_3 (b->handle_protocol, handle, (grub_efi_guid_t *) &dp_guid,
                       (void **) &device_path);
  if (status != GRUB_EFI_SUCCESS || !device_path)
    {
      grub_error (GRUB_ERR_BAD_DEVICE, "failed to query device path protocol");
      return;
    }

  status = efi_call_3 (b->handle_protocol, handle, (grub_efi_guid_t *) &blk_io_guid,
                       (void **) &block_io);
  if (status != GRUB_EFI_SUCCESS || !block_io)
    {
      grub_error (GRUB_ERR_BAD_DEVICE, "failed to query block io protocol");
      return;
    }

  efi_call_6 (b->uninstall_multiple_protocol_interfaces,
              handle, &dp_guid, device_path, &blk_io_guid, block_io, NULL);
}

/* Firmware protocols and their backing buffers have the same PUBLISH
   lifetime; EFI Map owns handles, whereas SOURCE/VIEW/FORMAT/BLOCK are common. */
static grub_err_t
grub_map_uninstall_handles (struct grub_efivdisk_data *d)
{
  grub_efi_boot_services_t *b = grub_efi_system_table->boot_services;
  grub_efi_status_t status;

  if (d->vpart.handle)
    {
      status = efi_call_6 (b->uninstall_multiple_protocol_interfaces,
                           d->vpart.handle, &dp_guid, d->vpart.dp,
                           &blk_io_guid, &d->vpart.block_io, NULL);
      if (status != GRUB_EFI_SUCCESS)
        return grub_error (GRUB_ERR_BAD_DEVICE,
                           "cannot uninstall EFI partition protocol");
      d->vpart.handle = 0;
    }
  if (d->vdisk.handle)
    {
      status = efi_call_6 (b->uninstall_multiple_protocol_interfaces,
                           d->vdisk.handle, &dp_guid, d->vdisk.dp,
                           &blk_io_guid, &d->vdisk.block_io, NULL);
      if (status != GRUB_EFI_SUCCESS)
        return grub_error (GRUB_ERR_BAD_DEVICE,
                           "cannot uninstall EFI disk protocol");
      d->vdisk.handle = 0;
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_map_stage_unpublish (struct grub_disk_pipeline *ctx,
                          const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  struct grub_efivdisk_data **link;
  struct grub_efivdisk_data *d = ctx->publish_data;
  grub_err_t err;

  for (link = &grub_efivdisk_list; *link; link = &(*link)->next)
    if ((*link)->pipeline == ctx && (*link)->refcount)
      return grub_error (GRUB_ERR_STILL_REFERENCED, "EFI virtual disk still referenced");

  err = grub_map_uninstall_handles (d);
  if (err)
    return err;

  /* El Torito aliases borrow the parent's pipeline and device handles. */
  for (link = &grub_efivdisk_list; *link;)
    {
      struct grub_efivdisk_data *item = *link;
      if (item->pipeline == ctx)
        {
          *link = item->next;
          if (!item->alias)
            {
              grub_free (item->vpart.dp);
              grub_free (item->vdisk.dp);
            }
          grub_free (item);
        }
      else
        link = &item->next;
    }
  ctx->publish_data = 0;
  ctx->published = 0;
  return GRUB_ERR_NONE;
}

static int
grub_map_stage_match (struct grub_disk_pipeline *ctx,
                      const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  return ctx->target == GRUB_DISK_TARGET_EFI;
}

static grub_err_t
grub_map_stage_publish (struct grub_disk_pipeline *ctx,
                        const struct grub_disk_hook *hook __attribute__ ((unused)))
{
  struct grub_efivdisk_data *d = ctx->publish_data;
  grub_err_t err;

  if (!d || !ctx->file || ctx->file->size == GRUB_FILE_SIZE_UNKNOWN)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "EFI map requires a sized disk");

  d->pipeline = ctx;
  d->vdisk.pipeline = ctx;
  d->vpart.pipeline = ctx;
  d->vdisk.file = ctx->file;
  d->vpart.file = ctx->file;
  d->vdisk.size = ctx->file->size;
  d->type = grub_vdisk_check_type (ctx->source, ctx->file, d->type);
  err = grub_efivdisk_install (d, d->install_state);
  d->install_state = 0;
  if (err)
    {
      grub_err_t cleanup = grub_map_uninstall_handles (d);
      if (cleanup)
        {
          /* EFI may still be holding one of the installed Block I/O
             protocols. Keep its backing pipeline reachable for unmap
             instead of freeing the firmware-visible pointers. */
          d->next = grub_efivdisk_list;
          grub_efivdisk_list = d;
          ctx->published = 1;
          return cleanup;
        }
      return err;
    }
  d->next = grub_efivdisk_list;
  grub_efivdisk_list = d;
  ctx->published = 1;
  return GRUB_ERR_NONE;
}

static struct grub_disk_hook grub_map_publish_hook =
  { .name = "efi-blockio", .stage = GRUB_DISK_PUBLISH,
    .probe = grub_map_stage_match, .run = grub_map_stage_publish,
    .unpublish = grub_map_stage_unpublish };

static void
unmap_efivdisk (grub_disk_t disk)
{
  struct grub_efivdisk_data *d;
  grub_efi_boot_services_t *b = grub_efi_system_table->boot_services;
  if (!disk || !disk->data)
    return;
  d = disk->data;
  if (d->vpart.handle)
    efi_call_6 (b->uninstall_multiple_protocol_interfaces,
                d->vpart.handle, &dp_guid, d->vpart.dp,
                &blk_io_guid, &d->vpart.block_io, NULL);
  efi_call_6 (b->uninstall_multiple_protocol_interfaces,
              d->vdisk.handle, &dp_guid, d->vdisk.dp,
              &blk_io_guid, &d->vdisk.block_io, NULL);
}

static grub_err_t
grub_efi_unmap_device (const char *name)
{
  struct grub_efivdisk_data **p;
  grub_disk_t disk = 0;

  for (p = &grub_efivdisk_list; *p; p = &(*p)->next)
    if (grub_strcmp ((*p)->devname, name) == 0)
      {
        struct grub_efivdisk_data *dev = *p;
        if (dev->refcount)
          return grub_error (GRUB_ERR_STILL_REFERENCED,
                             "EFI disk is still referenced");
        if (dev->alias)
          {
            *p = dev->next;
            grub_free (dev);
            return GRUB_ERR_NONE;
          }
        return grub_disk_pipeline_close (dev->pipeline);
      }
  disk = grub_disk_open (name);
  if (!disk)
    return grub_error (GRUB_ERR_BAD_DEVICE, "failed to open disk %s.", name);
  if (grub_strcmp (disk->dev->name, "efidisk") == 0)
    unmap_efidisk (disk);
  else if (grub_strcmp (disk->dev->name, "efivdisk") == 0)
    unmap_efivdisk (disk);
  else
    {
      grub_err_t err = grub_error (GRUB_ERR_BAD_DEVICE,
                                  "invalid disk: %s", disk->dev->name);
      grub_disk_close (disk);
      return err;
    }
  grub_disk_close (disk);
  return grub_errno;
}

static int
grub_efivdisk_iterate (grub_disk_dev_iterate_hook_t hook, void *hook_data,
                       grub_disk_pull_t pull)
{
  struct grub_efivdisk_data *d;
  if (pull != GRUB_DISK_PULL_NONE)
    return 0;
  for (d = grub_efivdisk_list; d; d = d->next)
  {
    if (hook (d->devname, hook_data))
      return 1;
  }
  return 0;
}

static grub_err_t
grub_efivdisk_open (const char *name, grub_disk_t disk)
{
  unsigned long i = 0;
  struct grub_efivdisk_data *dev;

  for (dev = grub_efivdisk_list; dev; dev = dev->next, i++)
    if (grub_strcmp (dev->devname, name) == 0)
      break;

  if (! dev)
    return grub_error (GRUB_ERR_UNKNOWN_DEVICE, "can't open device");

  /* Use the filesize for the disk size, round up to a complete sector.  */
  if (dev->vdisk.size != GRUB_FILE_SIZE_UNKNOWN)
    disk->total_sectors = (dev->vdisk.size + GRUB_DISK_SECTOR_SIZE - 1)
                          >> GRUB_DISK_SECTOR_BITS;
  else
    disk->total_sectors = GRUB_DISK_SIZE_UNKNOWN;
  /* Avoid reading more than 512M.  */
  disk->max_agglomerate = 1 << (29 - GRUB_DISK_SECTOR_BITS
                          - GRUB_DISK_CACHE_BITS);

  disk->id = i;

  disk->data = dev;
  dev->refcount++;

  return 0;
}

static void
grub_efivdisk_close (grub_disk_t disk)
{
  struct grub_efivdisk_data *dev = disk->data;
  if (dev && dev->refcount)
    dev->refcount--;
}

static grub_err_t
grub_efivdisk_read (grub_disk_t disk, grub_disk_addr_t sector,
                    grub_size_t size, char *buf)
{
  grub_file_t file = ((struct grub_efivdisk_data *) disk->data)->vdisk.file;
  grub_off_t start = ((struct grub_efivdisk_data *) disk->data)->vdisk.addr;
  grub_off_t len = ((struct grub_efivdisk_data *) disk->data)->vdisk.size;
  grub_off_t pos = (sector + size) << GRUB_DISK_SECTOR_BITS;

  if (((struct grub_efivdisk_data *) disk->data)->pipeline)
    return grub_disk_pipeline_read
      (((struct grub_efivdisk_data *) disk->data)->pipeline,
       (sector << GRUB_DISK_SECTOR_BITS) + start,
       size << GRUB_DISK_SECTOR_BITS, buf);
  file_read (file, buf, size << GRUB_DISK_SECTOR_BITS,
             (sector << GRUB_DISK_SECTOR_BITS) + start);

  if (grub_errno)
    return grub_errno;
  /* In case there is more data read than there is available, in case
     of files that are not a multiple of GRUB_DISK_SECTOR_SIZE, fill
     the rest with zeros.  */
  if (pos > len)
  {
    grub_size_t amount = pos - len;
    grub_memset (buf + (size << GRUB_DISK_SECTOR_BITS) - amount, 0, amount);
  }
  return 0;
}

static grub_err_t
grub_efivdisk_write (grub_disk_t disk, grub_disk_addr_t sector,
                     grub_size_t size, const char *buf)
{
  grub_file_t file = ((struct grub_efivdisk_data *) disk->data)->vdisk.file;
  grub_off_t start = ((struct grub_efivdisk_data *) disk->data)->vdisk.addr;
  if (((struct grub_efivdisk_data *) disk->data)->pipeline)
    return grub_disk_pipeline_write
      (((struct grub_efivdisk_data *) disk->data)->pipeline,
       (sector << GRUB_DISK_SECTOR_BITS) + start,
       size << GRUB_DISK_SECTOR_BITS, buf);
  file_write (file, buf, size << GRUB_DISK_SECTOR_BITS,
              (sector << GRUB_DISK_SECTOR_BITS) + start);
  return grub_errno;
}

static struct grub_disk_dev grub_efivdisk_dev =
{
  .name = "efivdisk",
  .id = GRUB_DISK_DEVICE_EFIVDISK_ID,
  .disk_iterate = grub_efivdisk_iterate,
  .disk_open = grub_efivdisk_open,
  .disk_close = grub_efivdisk_close,
  .disk_read = grub_efivdisk_read,
  .disk_write = grub_efivdisk_write,
  .next = 0
};

static void
grub_efivdisk_append (struct grub_efivdisk_data *disk)
{
  disk->next = grub_efivdisk_list;
  grub_efivdisk_list = disk;
}

static grub_err_t
mount_eltorito (struct grub_efivdisk_data *src, const char *name)
{
  struct grub_efivdisk_data *dst = NULL;
  grub_off_t ofs, len;

  if (!grub_iso_get_eltorito (src->vdisk.file, &ofs, &len))
    return grub_error (GRUB_ERR_FILE_READ_ERROR, "eltorito image not found");;
  dst = grub_zalloc (sizeof (struct grub_efivdisk_data));
  if (!dst)
    return grub_error (GRUB_ERR_BAD_OS, "out of memory");

  grub_printf ("Found UEFI El Torito image at %"
               PRIuGRUB_UINT64_T"+%"PRIuGRUB_UINT64_T"\n", ofs, len);
  grub_memcpy (dst, src, sizeof (struct grub_efivdisk_data));
  dst->alias = 1;
  dst->refcount = 0;
  dst->install_state = 0;
  dst->type = FD;
  dst->vpart.size = len;
  dst->vpart.addr = ofs;
  grub_memcpy (&dst->vdisk, &dst->vpart, sizeof (grub_efivdisk_t));
  grub_snprintf (dst->devname, 20, "%s", name);
  last_id++;
  grub_efivdisk_append (dst);

  return GRUB_ERR_NONE;
}

static const struct grub_arg_option options_map[] =
{
  {"mem", 'm', 0, N_("Copy to RAM."), 0, 0},
  {"blocklist", 'l', 0, N_("Convert to blocklist."), 0, 0},
  {"type", 't', 0, N_("Specify the disk type."), N_("CD/HD/FD"), ARG_TYPE_STRING},
  {"rt", 0, 0, N_("Set memory type to RESERVED_MEMORY_TYPE."), 0, 0},
  {"ro", 'o', 0, N_("Disable write support."), 0, 0},

  {"eltorito", 'e', 0,
    N_("Mount UEFI Eltorito image at the same time."), N_("disk"), ARG_TYPE_STRING},
  {"nb", 'n', 0, N_("Don't boot virtual disk."), 0, 0},
  {"unmap", 'x', 0, N_("Unmap devices."), N_("disk"), ARG_TYPE_STRING},
  {"first", 'f', 0, N_("Set as the first drive."), 0, 0},

  {"no_g4d", 'g', 0, N_("Don't write GRUB4DOS drive map info."), 0, 0},
  {"no_vt", 'v', 0, N_("Don't write Ventoy compatible info."), 0, 0},
  {"vtoy", 0, 0, N_("Set vtoy ACPI param."), 0, 0},
  {"cow", 'C', 0, N_("Use volatile copy-on-write disk blocks."), 0, 0},
  {"format", 'F', 0, N_("Parse a VHD/VHDX/QCOW2/VMDK/VDI container."), 0, 0},
#if 0
  {"alt", 'a', 0, N_("Install blockio using alternative methods."), 0, 0},
#endif
  {0, 0, 0, 0, 0, 0}
};

static grub_err_t
grub_cmd_map (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  struct grub_efivdisk_data *disk = NULL;
  struct grub_disk_pipeline *pipeline;
  unsigned int flags = GRUB_DISK_PIPELINE_DECOMPRESS;
  grub_err_t err;
  if (state[MAP_UNMAP].set)
  {
    if (state[MAP_UNMAP].arg[0] == '(')
    {
      state[MAP_UNMAP].arg[grub_strlen(state[MAP_UNMAP].arg) - 1] = '\0';
      grub_efi_unmap_device (&state[MAP_UNMAP].arg[1]);
    }
    else
      grub_efi_unmap_device (state[MAP_UNMAP].arg);
    return grub_errno;
  }
  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
  disk = grub_zalloc (sizeof (struct grub_efivdisk_data));
  if (!disk)
    return grub_errno;

  disk->type = UNKNOWN;
  if (state[MAP_TYPE].set)
    {
      if (state[MAP_TYPE].arg[0] == 'C' || state[MAP_TYPE].arg[0] == 'c')
        disk->type = CD;
      else if (state[MAP_TYPE].arg[0] == 'H' || state[MAP_TYPE].arg[0] == 'h')
        disk->type = HD;
      else if (state[MAP_TYPE].arg[0] == 'F' || state[MAP_TYPE].arg[0] == 'f')
        disk->type = FD;
    }
  if (argc < 2)
    grub_snprintf (disk->devname, sizeof (disk->devname), "vd%u", last_id);
  else
    grub_snprintf (disk->devname, sizeof (disk->devname), "%s", args[1]);

  if (state[MAP_MEM].set)
    flags |= GRUB_DISK_PIPELINE_MEMORY;
  if (state[MAP_BLOCK].set)
    flags |= GRUB_DISK_PIPELINE_BLOCKLIST;
  if (state[MAP_RT].set)
    flags |= GRUB_DISK_PIPELINE_RESERVED_MEMORY;
  if (state[MAP_RO].set)
    flags |= GRUB_DISK_PIPELINE_READ_ONLY;
  if (state[MAP_COW].set)
    flags |= GRUB_DISK_PIPELINE_COW;
  if (state[MAP_FORMAT].set)
    {
      if (!grub_disk_stage_available (GRUB_DISK_FORMAT)
          && !grub_dl_load ("vhd"))
        {
          grub_free (disk);
          return grub_errno;
        }
      flags |= GRUB_DISK_PIPELINE_FORMAT;
    }

  pipeline = grub_disk_pipeline_new (args[0], disk->devname,
                                    GRUB_DISK_TARGET_EFI, flags);
  if (!pipeline)
    {
      grub_free (disk);
      return grub_errno;
    }
  disk->install_state = state;
  pipeline->publish_data = disk;
  if (state[MAP_MEM].set)
    {
      pipeline->allocate_memory = grub_map_source_allocate;
      pipeline->release_memory = grub_map_source_release;
    }
  grub_guidgen (&disk->guid);
  err = grub_disk_pipeline_run (pipeline);
  if (err)
    {
      /* Incomplete EFI protocol rollback keeps a published device alive
         for a later explicit unmap; freeing it here would cause a UAF. */
      if (pipeline->published)
        return err;
      grub_disk_pipeline_close (pipeline);
      grub_free (disk->vpart.dp);
      grub_free (disk->vdisk.dp);
      grub_free (disk);
      return err;
    }
  last_id++;
  if (disk->type == CD && ! state[MAP_NOVT].set)
    grub_ventoy_set_osparam (args[0]);
  if (! state[MAP_NOG4D].set)
    g4d_add_drive (disk->vdisk.file, disk->type == CD);

  if (disk->type == CD && state[MAP_ELT].set)
    mount_eltorito (disk, state[MAP_ELT].arg);

  if (state[MAP_FIRST].set)
    grub_efi_set_first_disk (disk->vdisk.handle);

  if (state[MAP_VTOY].set && disk->type != CD)
    grub_ventoy_set_acpi_osparam (args[0]);

  if (state[MAP_NB].set)
    return grub_errno;

  /* load image */
  if (disk->vpart.dp)
    boot_image_handle = grub_efi_bootpart (disk->vpart.dp,
                                           EFI_REMOVABLE_MEDIA_FILE_NAME);
  if (!boot_image_handle)
    boot_image_handle = grub_efi_bootdisk (disk->vdisk.dp,
                                           EFI_REMOVABLE_MEDIA_FILE_NAME);
  if (!boot_image_handle)
    boot_image_handle = grub_efi_bootpart (disk->vdisk.dp,
                                           EFI_REMOVABLE_MEDIA_FILE_NAME);
  if (boot_image_handle)
  {
    grub_loader_set (grub_efiloader_boot, grub_efiloader_unload,
                     GRUB_LOADER_FLAG_EFI_KEEP_ALLOCATED_MEMORY);
    return GRUB_ERR_NONE;
  }
  return GRUB_ERR_FILE_NOT_FOUND;
}

static const struct grub_arg_option options_iso[] =
{
  {"offset", 'o', 0, N_("Offset of UEFI El Torito image (in sector unit)."), 0, 0},
  {"length", 'l', 0, N_("Size of UEFI El Torito image (in sector unit)."), 0, 0},
  {"ventoy", 'v', 0, N_("Check for whether ISO is ventoy compatible."), 0, 0},

  {0, 0, 0, 0, 0, 0}
};

enum options_iso
{
  ISO_OFS,
  ISO_LEN,
  ISO_VT,
};

static grub_err_t
grub_cmd_iso (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  grub_file_t file = 0;
  int ret = 0;
  char str[32];
  grub_off_t ofs = 0, len = 0;
  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
  if (argc < 2 && (state[ISO_OFS].set || state[ISO_LEN].set))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("varname expected"));
  file = file_open (args[0], 0, 0, 0);
  if (!file)
    return grub_error (GRUB_ERR_FILE_READ_ERROR, "failed to open file");

  if (state[ISO_VT].set)
    ret = grub_iso_check_vt (file);
  else
    ret = grub_iso_get_eltorito (file, &ofs, &len);

  if (state[ISO_OFS].set)
  {
    grub_snprintf (str, 32, "%"PRIuGRUB_UINT64_T, ofs >> GRUB_DISK_SECTOR_BITS);
    grub_env_set (args[1], str);
  }
  if (state[ISO_LEN].set)
  {
    grub_snprintf (str, 32, "%"PRIuGRUB_UINT64_T, len >> GRUB_DISK_SECTOR_BITS);
    grub_env_set (args[1], str);
  }
  return (ret ? GRUB_ERR_NONE : GRUB_ERR_TEST_FAILURE);
}

static grub_extcmd_t cmd_map, cmd_iso;

GRUB_MOD_INIT(map)
{
  grub_disk_hook_register (&grub_map_publish_hook, mod);
  cmd_map = grub_register_extcmd ("map", grub_cmd_map, 0, N_("FILE [DISK NAME]"),
                                  N_("Create virtual disk."), options_map);
  cmd_iso = grub_register_extcmd ("isotools", grub_cmd_iso, 0,
                                  N_("[-o|-l] FILE [VARNAME]"),
                                  N_("ISO tools."), options_iso);
  grub_disk_dev_register (&grub_efivdisk_dev);
}

GRUB_MOD_FINI(map)
{
  grub_disk_hook_unregister (&grub_map_publish_hook);
  grub_unregister_extcmd (cmd_map);
  grub_unregister_extcmd (cmd_iso);
  grub_disk_dev_unregister (&grub_efivdisk_dev);
}
