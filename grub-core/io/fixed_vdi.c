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

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/vdisk.h>

GRUB_MOD_LICENSE ("GPLv3+");

int grub_fixed_vdiio_probe (grub_file_t io, enum grub_file_type type);
grub_file_t grub_fixed_vdiio_open_filter (grub_file_t io, enum grub_file_type type);

#define VDI_IMAGE_SIGNATURE   0xbeda107f
#define VDI_HEADER_SIZE       0x178
#define VDI_HEADER_OFFSET     0x48
#define VDI_IMAGE_TYPE_OFFSET 0x4c
#define VDI_DATA_OFFSET       0x158
#define VDI_DISK_SIZE         0x170

struct grub_fixed_vdiio
{
  struct grub_vdisk disk;
  grub_file_t file;
  grub_uint64_t data_offset;
};
typedef struct grub_fixed_vdiio *grub_fixed_vdiio_t;

static grub_ssize_t
grub_fixed_vdiio_read (struct grub_vdisk *disk, grub_off_t off,
                       char *buf, grub_size_t len);

static grub_uint32_t
grub_fixed_vdiio_u32 (const grub_uint8_t *p)
{
  return (grub_uint32_t) p[0] | ((grub_uint32_t) p[1] << 8)
    | ((grub_uint32_t) p[2] << 16) | ((grub_uint32_t) p[3] << 24);
}

static grub_uint64_t
grub_fixed_vdiio_u64 (const grub_uint8_t *p)
{
  return (grub_uint64_t) grub_fixed_vdiio_u32 (p)
    | ((grub_uint64_t) grub_fixed_vdiio_u32 (p + 4) << 32);
}

static int
grub_fixed_vdiio_header (grub_file_t io, grub_uint64_t *data_offset,
                         grub_uint64_t *disk_size)
{
  grub_uint8_t hdr[VDI_HEADER_SIZE];
  grub_uint32_t image_type;
  grub_uint64_t end;

  if (!grub_vdisk_filter_should_open (io, GRUB_FILE_TYPE_LOOPBACK,
                                      VDI_HEADER_SIZE))
    return 0;
  if (!grub_vdisk_read_exact (io, 0, hdr, sizeof (hdr)))
    return 0;
  /* The leading 64-byte comment is freely chosen by the creator: QEMU and
     VirtualBox use different strings for valid VDI files.  The signature,
     declared header size and image type are the format identity. */
  if (grub_fixed_vdiio_u32 (hdr + 64) != VDI_IMAGE_SIGNATURE)
    return 0;

  image_type = grub_fixed_vdiio_u32 (hdr + VDI_IMAGE_TYPE_OFFSET);
  *data_offset = grub_fixed_vdiio_u32 (hdr + VDI_DATA_OFFSET);
  *disk_size = grub_fixed_vdiio_u64 (hdr + VDI_DISK_SIZE);
  if (image_type != 2
      || grub_fixed_vdiio_u32 (hdr + VDI_HEADER_OFFSET) <
         VDI_HEADER_SIZE - VDI_HEADER_OFFSET
      || *data_offset < VDI_HEADER_SIZE
      || *data_offset > io->size
      || *disk_size > io->size - *data_offset
      || *disk_size < 0x200)
    return 0;

  end = *data_offset + *disk_size;
  return end <= io->size;
}

static void
grub_fixed_vdiio_destroy (struct grub_vdisk *disk)
{
  grub_fixed_vdiio_t fixed_vdiio = (grub_fixed_vdiio_t) disk;

  if (disk->owns_backing && fixed_vdiio->file)
    grub_file_close (fixed_vdiio->file);
  grub_free (fixed_vdiio);
}

int
grub_fixed_vdiio_probe (grub_file_t io, enum grub_file_type type)
{
  grub_uint64_t data_offset, disk_size;

  if ((type & GRUB_FILE_TYPE_MASK) != GRUB_FILE_TYPE_LOOPBACK
      || type & GRUB_FILE_TYPE_NO_DECOMPRESS)
    return 0;
  return grub_fixed_vdiio_header (io, &data_offset, &disk_size);
}

grub_file_t
grub_fixed_vdiio_open_filter (grub_file_t io, enum grub_file_type type)
{
  grub_file_t file;
  grub_fixed_vdiio_t fixed_vdiio;
  grub_uint64_t data_offset, disk_size;
  grub_uint8_t mbr[2];

  if (!grub_fixed_vdiio_probe (io, type))
    return 0;

  if (!grub_fixed_vdiio_header (io, &data_offset, &disk_size)
      || !grub_vdisk_read_exact (io, data_offset + 0x1fe, mbr, sizeof (mbr)))
    return 0;
  if (mbr[0] != 0x55 || mbr[1] != 0xaa)
    return 0;

  file = grub_vdisk_open (sizeof (*fixed_vdiio),
                          (struct grub_vdisk **) &fixed_vdiio,
                          io, disk_size, GRUB_DISK_SECTOR_BITS,
                          grub_fixed_vdiio_read, grub_fixed_vdiio_destroy,
                          "fixed_vdi");
  if (!file)
    return 0;

  fixed_vdiio->file = io;
  fixed_vdiio->data_offset = data_offset;

  return file;
}

static grub_ssize_t
grub_fixed_vdiio_read (struct grub_vdisk *disk, grub_off_t off,
                       char *buf, grub_size_t len)
{
  grub_fixed_vdiio_t fixed_vdiio = (grub_fixed_vdiio_t) disk;
  grub_ssize_t ret;

  if ((grub_uint64_t) off > (~(grub_uint64_t) 0) - fixed_vdiio->data_offset)
    return -1;
  grub_file_seek (fixed_vdiio->file, off + fixed_vdiio->data_offset);
  if (grub_errno != GRUB_ERR_NONE)
    return -1;
  ret = grub_file_read (fixed_vdiio->file, buf, len);
  return ret;
}
