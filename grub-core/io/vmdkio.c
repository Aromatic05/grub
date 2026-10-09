/*
 *  GRUB  --  GRand Unified Bootloader
 *
 *  Read-only VMDK parser (monolithic sparse extent).
 */

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/vdisk.h>

GRUB_MOD_LICENSE ("GPLv3+");

int grub_vmdkio_probe (grub_file_t io, enum grub_file_type type);
grub_file_t grub_vmdkio_open_filter (grub_file_t io, enum grub_file_type type);

#define VMDK_SPARSE_MAGIC            0x564d444bU /* "KDMV" on disk */
#define VMDK_SECTOR_SIZE             512U

struct vmdk_sparse_header
{
  grub_uint32_t magic;
  grub_uint32_t version;
  grub_uint32_t flags;
  grub_uint64_t capacity;
  grub_uint64_t grain_size;
  grub_uint64_t descriptor_offset;
  grub_uint64_t descriptor_size;
  grub_uint32_t num_gtes_per_gt;
  grub_uint64_t rgd_offset;
  grub_uint64_t gd_offset;
  grub_uint64_t overhead;
  grub_uint8_t unclean_shutdown;
  grub_uint8_t single_end_line_char;
  grub_uint8_t non_end_line_char;
  grub_uint8_t double_end_line_char1;
  grub_uint8_t double_end_line_char2;
  grub_uint16_t compress_algorithm;
} GRUB_PACKED;

struct vmdk_ctx
{
  grub_file_t file;
  grub_uint64_t capacity_sectors;
  grub_uint32_t grain_size_sectors;
  grub_uint32_t num_gtes_per_gt;
  grub_uint64_t gd_offset_sectors;

  grub_uint32_t num_grains;
  grub_uint32_t num_gts;

  grub_uint32_t *gd;

  grub_uint32_t *gt_cache;
  grub_uint32_t gt_cache_index;
  int gt_cache_valid;
};

struct grub_vmdkio
{
  struct grub_vdisk disk;
  struct vmdk_ctx ctx;
};

typedef struct grub_vmdkio *grub_vmdkio_t;

static grub_uint16_t
rd_le16 (const void *p)
{
  grub_uint16_t v;
  grub_memcpy (&v, p, sizeof (v));
  return grub_le_to_cpu16 (v);
}

static grub_uint32_t
rd_le32 (const void *p)
{
  grub_uint32_t v;
  grub_memcpy (&v, p, sizeof (v));
  return grub_le_to_cpu32 (v);
}

static grub_uint64_t
rd_le64 (const void *p)
{
  grub_uint64_t v;
  grub_memcpy (&v, p, sizeof (v));
  return grub_le_to_cpu64 (v);
}

static grub_ssize_t
grub_vmdkio_read (struct grub_vdisk *disk, grub_off_t off,
                  char *buf, grub_size_t len);

static void
grub_vmdkio_destroy (struct grub_vdisk *disk)
{
  grub_vmdkio_t vmdkio = (grub_vmdkio_t) disk;

  grub_free (vmdkio->ctx.gt_cache);
  grub_free (vmdkio->ctx.gd);
  if (disk->owns_backing && vmdkio->ctx.file)
    grub_file_close (vmdkio->ctx.file);
  grub_free (vmdkio);
}

static int
vmdk_load_gt (struct vmdk_ctx *ctx, grub_uint32_t gt_index)
{
  grub_uint64_t gt_sector;
  grub_uint64_t gt_off;
  grub_uint64_t gt_size;
  grub_uint32_t i;

  if (ctx->gt_cache_valid && ctx->gt_cache_index == gt_index)
    return 1;

  if (gt_index >= ctx->num_gts)
    return 0;

  gt_sector = ctx->gd[gt_index];
  if (gt_sector == 0)
    {
      grub_memset (ctx->gt_cache, 0, ctx->num_gtes_per_gt * sizeof (grub_uint32_t));
      ctx->gt_cache_index = gt_index;
      ctx->gt_cache_valid = 1;
      return 1;
    }

  gt_off = gt_sector * (grub_uint64_t) VMDK_SECTOR_SIZE;
  gt_size = (grub_uint64_t) ctx->num_gtes_per_gt * sizeof (grub_uint32_t);

  if (gt_off + gt_size > ctx->file->size)
    return 0;

  if (!grub_vdisk_read_exact (ctx->file, gt_off, ctx->gt_cache, gt_size))
    return 0;

  for (i = 0; i < ctx->num_gtes_per_gt; i++)
    ctx->gt_cache[i] = grub_le_to_cpu32 (ctx->gt_cache[i]);

  ctx->gt_cache_index = gt_index;
  ctx->gt_cache_valid = 1;
  return 1;
}

int
grub_vmdkio_probe (grub_file_t io, enum grub_file_type type)
{
  grub_uint8_t hdr_raw[12];

  if (!grub_vdisk_filter_should_open (io, type, (grub_off_t) 512))
    return 0;

  if (!grub_vdisk_read_exact (io, 0, hdr_raw, sizeof (hdr_raw)))
    return 0;

  return (rd_le32 (hdr_raw + 0) == VMDK_SPARSE_MAGIC
          && rd_le32 (hdr_raw + 4) != 0
          && rd_le32 (hdr_raw + 8) != 0);
}

grub_file_t
grub_vmdkio_open_filter (grub_file_t io, enum grub_file_type type)
{
  grub_uint8_t hdr_raw[512];
  struct vmdk_sparse_header h;
  grub_file_t file = 0;
  grub_vmdkio_t vmdkio = 0;
  struct vmdk_ctx *ctx;
  grub_uint64_t gd_size;
  grub_uint32_t i;

  if (!grub_vmdkio_probe (io, type))
    return 0;

  if (!grub_vdisk_read_exact (io, 0, hdr_raw, sizeof (hdr_raw)))
    return 0;

  h.magic = rd_le32 (hdr_raw + 0);
  h.version = rd_le32 (hdr_raw + 4);
  h.flags = rd_le32 (hdr_raw + 8);
  h.capacity = rd_le64 (hdr_raw + 12);
  h.grain_size = rd_le64 (hdr_raw + 20);
  h.descriptor_offset = rd_le64 (hdr_raw + 28);
  h.descriptor_size = rd_le64 (hdr_raw + 36);
  h.num_gtes_per_gt = rd_le32 (hdr_raw + 44);
  h.rgd_offset = rd_le64 (hdr_raw + 48);
  h.gd_offset = rd_le64 (hdr_raw + 56);
  h.overhead = rd_le64 (hdr_raw + 64);
  h.unclean_shutdown = hdr_raw[72];
  h.single_end_line_char = hdr_raw[73];
  h.non_end_line_char = hdr_raw[74];
  h.double_end_line_char1 = hdr_raw[75];
  h.double_end_line_char2 = hdr_raw[76];
  h.compress_algorithm = rd_le16 (hdr_raw + 77);

  /* Only handle non-compressed sparse extents for now. */
  if (h.compress_algorithm != 0)
    {
      grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                  "compressed/streamOptimized VMDK is unsupported");
      return 0;
    }

  if (h.capacity == 0 || h.grain_size == 0 || h.num_gtes_per_gt == 0)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid VMDK sparse header");
      return 0;
    }

  if (h.grain_size > 0xffffffffU)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "VMDK grain size is too large");
      return 0;
    }

  /* Typical values are power-of-two, enforce for simpler mapping. */
  if ((h.grain_size & (h.grain_size - 1)) != 0)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "unsupported VMDK grain geometry");
      return 0;
    }

  if ((h.num_gtes_per_gt & (h.num_gtes_per_gt - 1)) != 0)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "unsupported VMDK GT geometry");
      return 0;
    }

  file = grub_vdisk_open (sizeof (*vmdkio), (struct grub_vdisk **) &vmdkio,
                          io,
                          h.capacity * (grub_uint64_t) VMDK_SECTOR_SIZE,
                          GRUB_DISK_SECTOR_BITS,
                          grub_vmdkio_read, grub_vmdkio_destroy, "vmdk");
  if (!file)
    return 0;

  ctx = &vmdkio->ctx;
  ctx->file = io;
  ctx->capacity_sectors = h.capacity;
  ctx->grain_size_sectors = (grub_uint32_t) h.grain_size;
  ctx->num_gtes_per_gt = h.num_gtes_per_gt;
  ctx->gd_offset_sectors = h.gd_offset;

  ctx->num_grains = grub_divmod64 (ctx->capacity_sectors + ctx->grain_size_sectors - 1,
				   ctx->grain_size_sectors, NULL);
  ctx->num_gts = grub_divmod64 (ctx->num_grains + ctx->num_gtes_per_gt - 1,
				ctx->num_gtes_per_gt, NULL);

  gd_size = (grub_uint64_t) ctx->num_gts * sizeof (grub_uint32_t);
  if (gd_size > (grub_uint64_t) (~(grub_size_t) 0))
    goto fail;

  if ((ctx->gd_offset_sectors * (grub_uint64_t) VMDK_SECTOR_SIZE) + gd_size > io->size)
    goto fail;

  ctx->gd = grub_malloc ((grub_size_t) gd_size);
  if (!ctx->gd)
    goto fail;

  if (!grub_vdisk_read_exact (io,
                              ctx->gd_offset_sectors * (grub_uint64_t) VMDK_SECTOR_SIZE,
                              ctx->gd,
                              (grub_size_t) gd_size))
    goto fail;

  for (i = 0; i < ctx->num_gts; i++)
    ctx->gd[i] = grub_le_to_cpu32 (ctx->gd[i]);

  ctx->gt_cache = grub_malloc ((grub_size_t) ctx->num_gtes_per_gt * sizeof (grub_uint32_t));
  if (!ctx->gt_cache)
    goto fail;
  ctx->gt_cache_valid = 0;
  ctx->gt_cache_index = 0;

  return file;

fail:
  grub_vdisk_fail (file, vmdkio ? &vmdkio->disk : 0);
  return 0;
}

static grub_ssize_t
grub_vmdkio_read (struct grub_vdisk *disk, grub_off_t off,
                  char *buf, grub_size_t len)
{
  grub_vmdkio_t vmdkio = (grub_vmdkio_t) disk;
  struct vmdk_ctx *ctx = &vmdkio->ctx;
  grub_size_t done = 0;

  if (off >= (grub_off_t) disk->size)
    return 0;
  if (off + len > (grub_off_t) disk->size)
    len = disk->size - off;

  while (len > 0)
    {
      grub_uint64_t cur = off;
      grub_uint64_t grain_bytes = (grub_uint64_t) ctx->grain_size_sectors * VMDK_SECTOR_SIZE;
      grub_uint64_t grain_index;
      grub_uint64_t in_grain;
      grub_uint64_t gte_index64;
      grub_uint32_t gt_index;
      grub_uint32_t gte_index;
      grub_uint32_t chunk;
      grub_uint64_t grain_sector;
      grub_uint64_t data_off;

      grain_index = grub_divmod64 (cur, grain_bytes, &in_grain);
      gt_index = grub_divmod64 (grain_index, ctx->num_gtes_per_gt, &gte_index64);
      gte_index = gte_index64;
      chunk = (grain_bytes - in_grain > len)
              ? (grub_uint32_t) len
              : (grub_uint32_t) (grain_bytes - in_grain);

      if (!vmdk_load_gt (ctx, gt_index))
        return grub_error (GRUB_ERR_READ_ERROR, "failed to load VMDK grain table");

      grain_sector = ctx->gt_cache[gte_index];
      if (grain_sector == 0)
        {
          grub_memset (buf, 0, chunk);
        }
      else
        {
          data_off = grain_sector * (grub_uint64_t) VMDK_SECTOR_SIZE + in_grain;
          if (data_off + chunk > ctx->file->size)
            return grub_error (GRUB_ERR_READ_ERROR, "VMDK grain points outside file");

          grub_file_seek (ctx->file, data_off);
          if (grub_file_read (ctx->file, buf, chunk) != (grub_ssize_t) chunk)
            return grub_error (GRUB_ERR_READ_ERROR, "failed to read VMDK grain data");
        }

      buf += chunk;
      off += chunk;
      len -= chunk;
      done += chunk;
    }

  return done;
}
