/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2020  Free Software Foundation, Inc.
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

#include <grub/types.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/err.h>
#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/device.h>
#include <grub/term.h>
#include <grub/partition.h>
#include <grub/file.h>
#include <grub/misc.h>
#include <grub/ventoy.h>
#include <grub/acpi.h>
#include <grub/script_sh.h>
#ifdef GRUB_MACHINE_EFI
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#else
#include <grub/relocator.h>
#endif

ventoy_os_param *
grub_ventoy_get_osparam (void)
{
  void *data = NULL;
#ifdef GRUB_MACHINE_EFI
  grub_guid_t ventoy_guid = VENTOY_GUID;
  grub_size_t datasize = 0;
  grub_efi_get_variable("VentoyOsParam", &ventoy_guid, &datasize, (void **) &data);
  if (!data || !datasize || datasize != sizeof (ventoy_os_param))
  {
    if (data)
      grub_free (data);
    return NULL;
  }
#else
  grub_addr_t addr = 0x80000;
  grub_packed_guid_t ventoy_guid = VENTOY_GUID;
  while (addr < 0xA0000)
  {
    if (grub_memcmp (&ventoy_guid, (void *)addr, sizeof (grub_packed_guid_t)) == 0)
    {
      data = (void *)addr;
      break;
    }
    addr++;
  }
  if (!data)
    return NULL;
#endif
  grub_printf ("VentoyOsParam found.\n");
  return data;
}

static int
ventoy_get_fs_type (const char *fs)
{
  if (!fs)
    return ventoy_fs_max;
  if (grub_strncmp(fs, "exfat", 5) == 0)
    return ventoy_fs_exfat;
  if (grub_strncmp(fs, "ntfs", 4) == 0)
    return ventoy_fs_ntfs;
  if (grub_strncmp(fs, "ext", 3) == 0)
    return ventoy_fs_ext;
  if (grub_strncmp(fs, "xfs", 3) == 0)
    return ventoy_fs_xfs;
  if (grub_strncmp(fs, "udf", 3) == 0)
    return ventoy_fs_udf;
  if (grub_strncmp(fs, "fat", 3) == 0)
    return ventoy_fs_fat;

  return ventoy_fs_max;
}

static int
ventoy_get_disk_guid (const char *filename, grub_uint8_t *guid,
                      grub_uint8_t *signature)
{
  grub_disk_t disk;
  char *device_name;
  char *pos;
  char *pos2;

  device_name = grub_file_get_device_name(filename);
  if (!device_name)
    return 1;

  pos = device_name;
  if (pos[0] == '(')
    pos++;

  pos2 = grub_strstr(pos, ",");
  if (!pos2)
    pos2 = grub_strstr(pos, ")");

  if (pos2)
    *pos2 = 0;

  disk = grub_disk_open(pos);
  grub_free(device_name);
  if (disk)
  {
    grub_disk_read(disk, 0, 0x180, 16, guid);
    grub_disk_read(disk, 0, 0x1b8, 4, signature);
    grub_disk_close(disk);
  }
  else
  {
    return 1;
  }
  return 0;
}

struct ventoy_block_range
{
  grub_disk_addr_t start_sector;
  grub_uint32_t num_sectors;
};

struct ventoy_block_ctx
{
  struct ventoy_block_range *ranges;
  grub_uint32_t used;
  grub_uint32_t cap;
  grub_disk_addr_t start_sector;
  grub_uint32_t num_sectors;
  int has_pending;
};

static grub_err_t
ventoy_append_range (struct ventoy_block_ctx *ctx,
                     grub_disk_addr_t start_sector, grub_uint32_t num_sectors)
{
  struct ventoy_block_range *new_ranges;
  grub_size_t new_cap;

  if (num_sectors == 0)
    return GRUB_ERR_NONE;

  if (ctx->used == ctx->cap)
    {
      new_cap = ctx->cap ? ctx->cap * 2 : 16;
      new_ranges = grub_realloc (ctx->ranges, new_cap * sizeof (ctx->ranges[0]));
      if (!new_ranges)
        return grub_errno;
      ctx->ranges = new_ranges;
      ctx->cap = new_cap;
    }

  ctx->ranges[ctx->used].start_sector = start_sector;
  ctx->ranges[ctx->used].num_sectors = num_sectors;
  ctx->used++;
  return GRUB_ERR_NONE;
}

static grub_err_t
ventoy_block_hook (grub_disk_addr_t sector, unsigned blk_offset, unsigned length,
                   char *buf __attribute__ ((unused)), void *data)
{
  struct ventoy_block_ctx *ctx = data;

  if (blk_offset != 0 || (length & (GRUB_DISK_SECTOR_SIZE - 1)))
    return GRUB_ERR_NONE;

  if (ctx->has_pending
      && ctx->start_sector + ctx->num_sectors == sector)
    {
      ctx->num_sectors += length >> GRUB_DISK_SECTOR_BITS;
      return GRUB_ERR_NONE;
    }

  if (ctx->has_pending)
    {
      grub_err_t err = ventoy_append_range (ctx, ctx->start_sector, ctx->num_sectors);
      if (err != GRUB_ERR_NONE)
        return err;
    }

  ctx->start_sector = sector;
  ctx->num_sectors = length >> GRUB_DISK_SECTOR_BITS;
  ctx->has_pending = 1;
  return GRUB_ERR_NONE;
}

void
grub_ventoy_fill_osparam (grub_file_t file, ventoy_os_param *param)
{
  char *pos;
  grub_uint32_t i;
  grub_uint8_t chksum = 0;
  grub_disk_t disk;
  const grub_packed_guid_t vtguid = VENTOY_GUID;

  /* This is a complete protocol record, including reserved fields and
     checksum. The callers use stack storage, not a zeroed allocation. */
  grub_memset (param, 0, sizeof (*param));
  disk = file->device->disk;
  grub_memcpy(&param->guid, &vtguid, sizeof(grub_packed_guid_t));

  param->vtoy_disk_size = disk->total_sectors * (1 << disk->log_sector_size);
  param->vtoy_disk_part_id = disk->partition ? disk->partition->number + 1 : 0;
  param->vtoy_disk_part_type = ventoy_get_fs_type(file->fs->name);

  pos = grub_strstr (file->name, "/");
  if (!pos)
    pos = file->name;

  grub_snprintf (param->vtoy_img_path, sizeof(param->vtoy_img_path), "%s", pos);

  ventoy_get_disk_guid(file->name, param->vtoy_disk_guid,
                       param->vtoy_disk_signature);

  param->vtoy_img_size = file->size;

  param->vtoy_reserved[0] = 0;
  param->vtoy_reserved[1] = 0;

  /* calculate checksum */
  for (i = 0; i < sizeof(ventoy_os_param); i++)
  {
    chksum += *((grub_uint8_t *)param + i);
  }
  param->chksum = (grub_uint8_t)(0x100 - chksum);

  return;
}

void
grub_ventoy_set_acpi_osparam (const char *filename)
{
  ventoy_os_param param, *osparam;
  grub_uint32_t i;
  grub_uint64_t part_start = 0, offset = 0;
  grub_file_t file = 0;
  ventoy_image_location *location;
  ventoy_image_disk_region *region;
  struct grub_acpi_table_header *acpi = 0;
  grub_uint64_t buflen, loclen;
  char cmd[64];
  grub_uint32_t max_chunk = 0;
  struct ventoy_block_ctx ctx = {0};

  file = grub_file_open (filename, GRUB_FILE_TYPE_GET_SIZE);
  if (!file || !file->device || !file->device->disk)
    {
      if (file)
        grub_file_close (file);
      return;
    }
  grub_ventoy_fill_osparam (file, &param);

  if (file->device->disk->partition)
    part_start = grub_partition_get_start (file->device->disk->partition);

  file->offset = 0;
  file->read_hook = ventoy_block_hook;
  file->read_hook_data = &ctx;
  {
    char tmp[GRUB_DISK_SECTOR_SIZE];
    while (grub_file_read (file, tmp, sizeof (tmp)) > 0)
      ;
  }

  if (ctx.has_pending)
    {
      if (ventoy_append_range (&ctx, ctx.start_sector, ctx.num_sectors) != GRUB_ERR_NONE)
        goto fail;
    }

  max_chunk = ctx.used;
  if (!max_chunk)
    goto fail;

  /* calculate acpi table length */
  loclen = sizeof (ventoy_image_location) +
           max_chunk * sizeof(ventoy_image_disk_region);
  buflen = sizeof (struct grub_acpi_table_header) +
           sizeof (ventoy_os_param) + loclen;
  acpi = grub_zalloc(buflen);
  if (!acpi)
    goto fail;
  /* Step1: Fill acpi table header */
  grub_memcpy (acpi->signature, "VTOY", 4);
  acpi->length = buflen;
  acpi->revision = 1;
  grub_memcpy (acpi->oemid, "VENTOY", 6);
  grub_memcpy (acpi->oemtable, "OSPARAMS", 8);
  acpi->oemrev = 1;
  acpi->creator_id[0] = 1;
  acpi->creator_rev = 1;
  /* Step2: Fill data */
  osparam = (ventoy_os_param *)(acpi + 1);
  grub_memcpy (osparam, &param, sizeof(ventoy_os_param));
  osparam->vtoy_img_location_addr = 0;
  osparam->vtoy_img_location_len  = loclen;
  osparam->chksum = 0;
  osparam->chksum = 0x100 - grub_byte_checksum (osparam, sizeof (ventoy_os_param));

  location = (ventoy_image_location *)(osparam + 1);
  grub_memcpy (&location->guid, &osparam->guid, sizeof (grub_packed_guid_t));
  location->image_sector_size = 512;
  location->disk_sector_size  = 512;
  location->region_count = max_chunk;
  region = location->regions;
  for (i = 0; i < max_chunk; i++, region++)
  {
    region->image_sector_count = ctx.ranges[i].num_sectors;
    region->image_start_sector = offset;
    region->disk_start_sector = ctx.ranges[i].start_sector + part_start;
    offset += region->image_sector_count;
    grub_printf ("add region: LBA=%llu IMG %llu+%llu\n",
                 (unsigned long long) region->disk_start_sector,
                 (unsigned long long) region->image_start_sector,
                 (unsigned long long) region->image_sector_count);
  }
  /* Step3: Fill acpi checksum */
  acpi->checksum = 0;
  acpi->checksum = 0x100 - grub_byte_checksum (acpi, acpi->length);

  /* load acpi table */
  grub_snprintf (cmd, sizeof(cmd), "acpi mem:%p:size:%u", acpi, acpi->length);
  grub_printf ("%s\n", cmd);
  grub_script_execute_sourcecode (cmd);
  grub_free (acpi);
#ifdef GRUB_MACHINE_EFI
  /* unset uefi var VentoyOsParam */
  grub_guid_t vtguid = VENTOY_GUID;
  grub_efi_set_variable_with_attributes ("VentoyOsParam", &vtguid, NULL, 0,
        GRUB_EFI_VARIABLE_BOOTSERVICE_ACCESS | GRUB_EFI_VARIABLE_RUNTIME_ACCESS);
#else
  grub_addr_t addr = 0x80000;
  grub_packed_guid_t vtguid = VENTOY_GUID;
  while (addr < 0xA0000)
  {
    if (grub_memcmp (&vtguid, (void *)addr, sizeof (vtguid)) == 0)
    {
      grub_memset((void *)addr, 0, sizeof (vtguid));
      break;
    }
    addr++;
  }
#endif
fail:
  grub_free (ctx.ranges);
  if (file)
    grub_file_close (file);
}

void
grub_ventoy_set_osparam (const char *filename)
{
  ventoy_os_param param;
  grub_file_t file = 0;
  file = grub_file_open (filename, GRUB_FILE_TYPE_GET_SIZE);
  if (!file || !file->device || !file->device->disk)
    goto fail;
  grub_ventoy_fill_osparam (file, &param);
#ifdef GRUB_MACHINE_EFI
  grub_guid_t vtguid = VENTOY_GUID;
  grub_efi_set_variable_with_attributes ("VentoyOsParam", &vtguid, &param, sizeof (param),
        GRUB_EFI_VARIABLE_BOOTSERVICE_ACCESS | GRUB_EFI_VARIABLE_RUNTIME_ACCESS);
#else
  void *data = NULL;
  grub_relocator_chunk_t ch;
  struct grub_relocator *relocator = NULL;
  data = grub_ventoy_get_osparam ();
  if (!data)
  {
    relocator = grub_relocator_new ();
    if (!relocator)
      goto fail;
    if (grub_relocator_alloc_chunk_align (relocator, &ch, 0x80000, 0xA0000,
                sizeof (param), 1, GRUB_RELOCATOR_PREFERENCE_LOW, 0))
    {
      grub_relocator_unload (relocator);
      goto fail;
    }
    data = get_virtual_current_address(ch);
    grub_relocator_unload (relocator);
  }
  grub_memcpy (data, &param, sizeof (param));
#endif
  grub_printf ("VentoyOsParam created.\n");
fail:
  if (file)
    grub_file_close (file);
}
