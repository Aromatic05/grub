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

#ifndef GRUB_MAPLIB_MISC_H
#define GRUB_MAPLIB_MISC_H

#include <stddef.h>
#include <stdint.h>

#include <grub/types.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/disk_pipeline.h>
#include <grub/extcmd.h>

#define CD_BOOT_SECTOR 17
#define CD_BLOCK_SIZE 2048
#define CD_SHIFT 11

#define FD_BLOCK_SIZE 512 /* 0x200 */
#define FD_SHIFT 9
#define BLOCK_OF_1_44MB 0xB40

#define MAX_FILE_NAME_STRING_SIZE 255
#define MBR_START_LBA 0
#define PRIMARY_PART_HEADER_LBA 1
#define VDISK_MEDIA_ID 0x1

#ifndef GRUB_DISK_DEVICE_EFIVDISK_ID
#define GRUB_DISK_DEVICE_EFIVDISK_ID 1000
#endif
#ifndef GRUB_DISK_DEVICE_VFAT_ID
#define GRUB_DISK_DEVICE_VFAT_ID 1001
#endif

#define PAGE_SIZE 4096
#define MBR_TYPE_PCAT 0x01
#define SIGNATURE_TYPE_MBR 0x01

#ifndef CR
#define CR(record, type, field) \
  ((type *) ((char *) (record) - offsetof (type, field)))
#endif

#ifdef GRUB_MACHINE_EFI

#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/efi/disk.h>

#ifndef EFIAPI
#define EFIAPI __grub_efi_api
#endif

#ifndef TRUE
#define TRUE  ((grub_efi_boolean_t) 1)
#endif
#ifndef FALSE
#define FALSE ((grub_efi_boolean_t) 0)
#endif

#ifndef efi_call_0
#define efi_call_0(func)                                (func)()
#define efi_call_1(func, a)                             (func)(a)
#define efi_call_2(func, a, b)                          (func)(a, b)
#define efi_call_3(func, a, b, c)                       (func)(a, b, c)
#define efi_call_4(func, a, b, c, d)                    (func)(a, b, c, d)
#define efi_call_5(func, a, b, c, d, e)                 (func)(a, b, c, d, e)
#define efi_call_6(func, a, b, c, d, e, f)              (func)(a, b, c, d, e, f)
#define efi_call_7(func, a, b, c, d, e, f, g)           (func)(a, b, c, d, e, f, g)
#endif

#ifndef EFI_REMOVABLE_MEDIA_FILE_NAME
#if defined (__i386__)
#define EFI_REMOVABLE_MEDIA_FILE_NAME "/EFI/BOOT/BOOTIA32.EFI"
#elif defined (__x86_64__)
#define EFI_REMOVABLE_MEDIA_FILE_NAME "/EFI/BOOT/BOOTX64.EFI"
#elif defined (__arm__)
#define EFI_REMOVABLE_MEDIA_FILE_NAME "/EFI/BOOT/BOOTARM.EFI"
#elif defined (__aarch64__)
#define EFI_REMOVABLE_MEDIA_FILE_NAME "/EFI/BOOT/BOOTAA64.EFI"
#endif
#endif

#ifndef HW_VENDOR_DP
#define HW_VENDOR_DP              0x04
#endif
#ifndef MEDIA_HARDDRIVE_DP
#define MEDIA_HARDDRIVE_DP        0x01
#endif
#ifndef MEDIA_CDROM_DP
#define MEDIA_CDROM_DP            0x02
#endif
#ifndef MEDIA_DEVICE_PATH
#define MEDIA_DEVICE_PATH         0x04
#endif
#ifndef HARDWARE_DEVICE_PATH
#define HARDWARE_DEVICE_PATH      0x01
#endif

#ifndef GRUB_EFI_COMPONENT_NAME_PROTOCOL_GUID
#define GRUB_EFI_COMPONENT_NAME_PROTOCOL_GUID \
  { 0x107a772c, 0xd5e1, 0x11d4, \
    { 0x9a, 0x46, 0x0, 0x90, 0x27, 0x3f, 0xc1, 0x4d } \
  }
#endif

#ifndef EFI_BLOCK_IO_PROTOCOL_REVISION
#define EFI_BLOCK_IO_PROTOCOL_REVISION  0x00010000
#endif

typedef grub_guid_t grub_efi_guid_t;
typedef grub_efi_block_io_t block_io_protocol_t;

extern block_io_protocol_t blockio_template;

typedef struct
{
  grub_efi_uint64_t addr;
  grub_efi_uint64_t size;
  grub_efi_handle_t handle;
  grub_efi_device_path_t *dp;
  block_io_protocol_t block_io;
  grub_efi_block_io_media_t media;
  grub_file_t file;
  struct grub_disk_pipeline *pipeline;
} grub_efivdisk_t;

enum grub_efivdisk_type
{
  UNKNOWN,
  HD,
  CD,
  FD,
  MBR,
  GPT,
};

struct grub_efivdisk_data
{
  char devname[20];
  enum grub_efivdisk_type type;
  grub_packed_guid_t guid;
  grub_efivdisk_t vdisk;
  grub_efivdisk_t vpart;
  struct grub_disk_pipeline *pipeline;
  struct grub_arg_list *install_state;
  unsigned int refcount;
  int alias;
  struct grub_efivdisk_data *next;
};

extern struct grub_efivdisk_data *grub_efivdisk_list;
void *grub_map_allocate_pages (grub_efi_uint64_t size,
                               grub_efi_memory_type_t memtype);

enum options_map
{
  MAP_MEM,
  MAP_BLOCK,
  MAP_TYPE,
  MAP_RT,
  MAP_RO,
  MAP_ELT,
  MAP_NB,
  MAP_UNMAP,
  MAP_FIRST,
  MAP_NOG4D,
  MAP_NOVT,
  MAP_VTOY,
  MAP_COW,
  MAP_FORMAT,
#if 0
  MAP_ALT,
#endif
};

enum grub_efivdisk_type
grub_vdisk_check_type (const char *name, grub_file_t file,
                       enum grub_efivdisk_type type);

grub_efi_status_t
grub_efivdisk_connect_driver (grub_efi_handle_t controller,
                              const grub_efi_char16_t *name);

grub_err_t
grub_efivdisk_install (struct grub_efivdisk_data *disk,
                       struct grub_arg_list *state);

grub_err_t
grub_efivpart_install (struct grub_efivdisk_data *disk,
                       struct grub_arg_list *state);

grub_efi_handle_t
grub_efi_bootpart (grub_efi_device_path_t *dp, const char *filename);

grub_efi_handle_t
grub_efi_bootdisk (grub_efi_device_path_t *dp, const char *filename);

grub_efi_device_path_protocol_t *
grub_efi_create_device_node (grub_efi_uint8_t node_type,
                             grub_efi_uintn_t node_subtype,
                             grub_efi_uint16_t node_length);

grub_efi_device_path_protocol_t *
grub_efi_append_device_path (const grub_efi_device_path_protocol_t *dp1,
                             const grub_efi_device_path_protocol_t *dp2);

grub_efi_device_path_protocol_t *
grub_efi_append_device_node (const grub_efi_device_path_protocol_t *device_path,
                             const grub_efi_device_path_protocol_t *device_node);

grub_efi_device_path_t *
grub_efi_file_device_path (grub_efi_device_path_t *dp, const char *filename);

int
grub_efi_is_child_dp (const grub_efi_device_path_t *child,
                      const grub_efi_device_path_t *parent);

static inline void
grub_efi_dprintf_dp (grub_efi_device_path_t *dp)
{
  grub_dprintf ("map", "dp=%p\n", dp);
}

void grub_efi_set_first_disk (grub_efi_handle_t handle);

#endif

wchar_t *grub_wstrstr (const wchar_t *str, const wchar_t *search_str);

void grub_pause_boot (void);

void grub_pause_fatal (const char *fmt, ...);

grub_file_t file_open (const char *name, int mem, int bl, int rt);

void file_read (grub_file_t file, void *buf, grub_size_t len, grub_off_t offset);

void
file_write (grub_file_t file, const void *buf, grub_size_t len, grub_off_t offset);

void file_close (grub_file_t file);

extern int grub_isefi;

#endif
