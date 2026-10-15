#ifndef __VENTOY_COMPAT_H__
#define __VENTOY_COMPAT_H__

#include <grub/types.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/env.h>
#include <grub/disk.h>
#include <grub/partition.h>

#ifdef GRUB_MACHINE_EFI
#include <grub/efi/efi.h>
#endif

#define VTOY_CHUNK_BUF_SIZE          (4 * 1024 * 1024)

#ifndef GRUB_FILE_TYPE_NO_VLNK
#define GRUB_FILE_TYPE_NO_VLNK 0
#endif

/* Compatibility aliases for current grub_port implementation names. */
#ifndef VTOY_OFFSET_OF
#define VTOY_OFFSET_OF(type, member) OFFSET_OF(type, member)
#endif

#define grub_ventoy_patch_vhd ventoy_patch_vhd
#define grub_ventoy_iso9660_override ventoy_iso9660_override
#define grub_ventoy_udf_override ventoy_udf_override
#define grub_ventoy_wim_stream_entry wim_stream_entry
#define grub_ventoy_windows_patch ventoy_windows_patch
#define grub_ventoy_windows_reg_vk reg_vk

typedef struct ventoy_windows_patch_blob_header
{
  grub_uint32_t total_patch_count;
  grub_uint32_t valid_patch_count;
  grub_uint32_t record_size;
  grub_uint32_t reserved;
} ventoy_windows_patch_blob_header;

typedef struct ventoy_windows_patch_blob_record
{
  grub_uint8_t valid;
  grub_uint8_t reserved0[3];
  grub_uint32_t pathlen;
  char path[256];
} ventoy_windows_patch_blob_record;

#define grub_ventoy_windows_patch_blob_header ventoy_windows_patch_blob_header
#define grub_ventoy_windows_patch_blob_record ventoy_windows_patch_blob_record

grub_fs_t
ventoy_compat_fs_list_probe (grub_device_t device, const char **list);

int
ventoy_compat_get_file_chunk (grub_uint64_t part_start, grub_file_t file,
                              ventoy_img_chunk_list *chunk_list);

void
ventoy_compat_env_hook_root (int hook);

grub_err_t
ventoy_compat_register_menu_lang_hook (grub_env_read_hook_t read_hook);

void *
ventoy_compat_env_get (const char *name);

void
ventoy_compat_iso9660_set_nojoliet (int nojoliet);

int
ventoy_compat_iso9660_is_joliet (void);

int
ventoy_check_file_exist (const char *fmt, ...);

int
grub_file_is_vlnk_suffix (const char *name, int len);

int
grub_file_add_vlnk (const char *src, const char *dst);

int
grub_file_vtoy_vlnk (const char *src, const char *dst);

const char *
grub_file_get_vlnk (const char *name, int *vlnk);

void
ventoy_compat_set_file_vlnk (grub_file_t file, int vlnk);

int
ventoy_compat_get_file_vlnk (grub_file_t file);

int
ventoy_compat_get_gpt_priority (grub_disk_t disk, grub_partition_t part,
                                grub_uint32_t *priority);

int
ventoy_compat_efi_secureboot_enabled (void);

static inline void *
ventoy_compat_efi_allocate_iso_buf (grub_uint64_t size)
{
#ifdef GRUB_MACHINE_EFI
  grub_efi_uintn_t pages = GRUB_EFI_BYTES_TO_PAGES (size);
  return grub_efi_allocate_pages_real (0, pages, GRUB_EFI_ALLOCATE_ANY_PAGES,
                                       GRUB_EFI_RUNTIME_SERVICES_DATA);
#else
  (void) size;
  return NULL;
#endif
}

static inline void *
ventoy_compat_efi_allocate_chain_buf (grub_uint64_t size)
{
#ifdef GRUB_MACHINE_EFI
  grub_efi_uintn_t pages = GRUB_EFI_BYTES_TO_PAGES (size);
  return grub_efi_allocate_pages_real (0, pages, GRUB_EFI_ALLOCATE_ANY_PAGES,
                                       GRUB_EFI_LOADER_DATA);
#else
  (void) size;
  return NULL;
#endif
}

static inline void
ventoy_compat_efi_get_reserved_page_num (grub_uint64_t *total,
                                         grub_uint64_t *org_required,
                                         grub_uint64_t *new_required)
{
  if (total)
    *total = 0;
  if (org_required)
    *org_required = 0;
  if (new_required)
    *new_required = 0;
}

static inline grub_uint64_t
ventoy_compat_dirent_size (const struct grub_dirhook_info *info)
{
  (void) info;
  return 0;
}

#endif
