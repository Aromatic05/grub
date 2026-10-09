/*
 *  GRUB  --  GRand Unified Bootloader
 *
 *  Read-only VHDX parser (fixed/dynamic, no parent chain).
 */

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/lib/crc.h>
#include <grub/vdisk.h>

GRUB_MOD_LICENSE ("GPLv3+");

int grub_vhdxio_probe (grub_file_t io, enum grub_file_type type);
grub_file_t grub_vhdxio_open_filter (grub_file_t io, enum grub_file_type type);

#define VHDX_FILE_ID_OFF              0x00000
#define VHDX_HEADER1_OFF              0x10000
#define VHDX_HEADER2_OFF              0x20000
#define VHDX_REGION_TBL1_OFF          0x30000
#define VHDX_REGION_TBL2_OFF          0x40000

#define VHDX_HEADER_BLOCK_SIZE        0x10000
#define VHDX_HEADER_SIZE              0x1000
#define VHDX_METADATA_TBL_SIZE        0x10000

#define VHDX_SIG_FILE                 "vhdxfile"
#define VHDX_SIG_HEAD                 "head"
#define VHDX_SIG_REGION               0x69676572U /* 'regi' */
#define VHDX_SIG_METADATA             "metadata"

#define VHDX_MAX_SECTORS_PER_BLOCK    (1U << 23)

#define VHDX_BAT_STATE_MASK           0x7ULL
#define VHDX_BAT_FILE_OFF_MASK        0xFFFFFFFFFFF00000ULL

#define VHDX_PAYLOAD_NOT_PRESENT      0
#define VHDX_PAYLOAD_UNDEFINED        1
#define VHDX_PAYLOAD_ZERO             2
#define VHDX_PAYLOAD_UNMAPPED         3
#define VHDX_PAYLOAD_UNMAPPED_V095    4
#define VHDX_PAYLOAD_UNMAPPED_V095_2  5
#define VHDX_PAYLOAD_FULL             6
#define VHDX_PAYLOAD_PARTIAL          7

#define VHDX_PARAM_HAS_PARENT         0x00000002U

struct vhdx_guid
{
  grub_uint32_t d1;
  grub_uint16_t d2;
  grub_uint16_t d3;
  grub_uint8_t d4[8];
};

struct vhdx_region_entry
{
  grub_uint64_t file_offset;
  grub_uint32_t length;
};

struct vhdx_meta_loc
{
  grub_uint32_t offset;
  grub_uint32_t length;
  int present;
};

struct vhdx_ctx
{
  grub_file_t file;
  grub_uint64_t virtual_size;
  grub_uint32_t block_size;
  grub_uint32_t logical_sector_size;
  grub_uint32_t chunk_ratio;

  grub_uint64_t *bat;
  grub_uint32_t bat_entries;
};

struct grub_vhdxio
{
  struct grub_vdisk disk;
  struct vhdx_ctx ctx;
};

typedef struct grub_vhdxio *grub_vhdxio_t;

static grub_uint16_t
rd16 (const void *p)
{
  grub_uint16_t v;
  grub_memcpy (&v, p, sizeof (v));
  return grub_le_to_cpu16 (v);
}

static grub_uint32_t
rd32 (const void *p)
{
  grub_uint32_t v;
  grub_memcpy (&v, p, sizeof (v));
  return grub_le_to_cpu32 (v);
}

static grub_uint64_t
rd64 (const void *p)
{
  grub_uint64_t v;
  grub_memcpy (&v, p, sizeof (v));
  return grub_le_to_cpu64 (v);
}

static grub_ssize_t
grub_vhdxio_read (struct grub_vdisk *disk, grub_off_t off,
                  char *buf, grub_size_t len);

static void
grub_vhdxio_destroy (struct grub_vdisk *disk)
{
  grub_vhdxio_t vhdxio = (grub_vhdxio_t) disk;

  grub_free (vhdxio->ctx.bat);
  if (disk->owns_backing && vhdxio->ctx.file)
    grub_file_close (vhdxio->ctx.file);
  grub_free (vhdxio);
}

static int
guid_eq (const grub_uint8_t *raw,
         grub_uint32_t d1, grub_uint16_t d2, grub_uint16_t d3,
         const grub_uint8_t d4[8])
{
  struct vhdx_guid g;

  g.d1 = rd32 (raw);
  g.d2 = rd16 (raw + 4);
  g.d3 = rd16 (raw + 6);
  grub_memcpy (g.d4, raw + 8, 8);

  if (g.d1 != d1 || g.d2 != d2 || g.d3 != d3)
    return 0;
  return grub_memcmp (g.d4, d4, 8) == 0;
}

static int
crc32c_valid (grub_uint8_t *buf, grub_size_t size, grub_size_t crc_off)
{
  grub_uint32_t orig;
  grub_uint32_t calc;

  if (crc_off + 4 > size)
    return 0;

  orig = rd32 (buf + crc_off);
  grub_memset (buf + crc_off, 0, 4);
  calc = grub_getcrc32c (0xffffffffU, buf, size);

  /* Restore original field bytes. */
  {
    grub_uint32_t le = grub_cpu_to_le32 (orig);
    grub_memcpy (buf + crc_off, &le, 4);
  }

  return calc == orig;
}

static int
parse_header_block (grub_uint8_t *blk, grub_uint64_t *seq)
{
  if (grub_memcmp (blk, VHDX_SIG_HEAD, 4) != 0)
    return 0;
  if (!crc32c_valid (blk, VHDX_HEADER_BLOCK_SIZE, 4))
    return 0;
  if (rd16 (blk + 68) != 1)
    return 0;

  *seq = rd64 (blk + 8);
  return 1;
}

static int
load_current_header (grub_file_t io, grub_uint8_t *out_header)
{
  grub_uint8_t *h1 = 0;
  grub_uint8_t *h2 = 0;
  grub_uint64_t s1 = 0, s2 = 0;
  int v1 = 0, v2 = 0;

  h1 = grub_malloc (VHDX_HEADER_BLOCK_SIZE);
  h2 = grub_malloc (VHDX_HEADER_BLOCK_SIZE);
  if (!h1 || !h2)
    goto fail;

  if (!grub_vdisk_read_exact (io, VHDX_HEADER1_OFF, h1, VHDX_HEADER_BLOCK_SIZE))
    goto fail;
  if (!grub_vdisk_read_exact (io, VHDX_HEADER2_OFF, h2, VHDX_HEADER_BLOCK_SIZE))
    goto fail;

  v1 = parse_header_block (h1, &s1);
  v2 = parse_header_block (h2, &s2);

  if (!v1 && !v2)
    goto fail;

  if (v1 && !v2)
    grub_memcpy (out_header, h1, VHDX_HEADER_SIZE);
  else if (!v1 && v2)
    grub_memcpy (out_header, h2, VHDX_HEADER_SIZE);
  else if (s1 > s2)
    grub_memcpy (out_header, h1, VHDX_HEADER_SIZE);
  else if (s2 > s1)
    grub_memcpy (out_header, h2, VHDX_HEADER_SIZE);
  else if (grub_memcmp (h1, h2, VHDX_HEADER_SIZE) == 0)
    grub_memcpy (out_header, h1, VHDX_HEADER_SIZE);
  else
    goto fail;

  grub_free (h1);
  grub_free (h2);
  return 1;

fail:
  grub_free (h1);
  grub_free (h2);
  return 0;
}

static int
parse_region_table_block (grub_uint8_t *blk,
                          struct vhdx_region_entry *bat,
                          struct vhdx_region_entry *meta)
{
  grub_uint32_t sig;
  grub_uint32_t i;
  grub_uint32_t cnt;
  int bat_found = 0;
  int meta_found = 0;

  static const grub_uint8_t BAT_D4[8] =
    {0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08};
  static const grub_uint8_t META_D4[8] =
    {0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e};

  sig = rd32 (blk + 0);
  if (sig != VHDX_SIG_REGION)
    return 0;
  if (!crc32c_valid (blk, VHDX_HEADER_BLOCK_SIZE, 4))
    return 0;

  cnt = rd32 (blk + 8);
  if (cnt > 2047)
    return 0;

  for (i = 0; i < cnt; i++)
    {
      const grub_uint8_t *e = blk + 16 + i * 32;
      grub_uint64_t off = rd64 (e + 16);
      grub_uint32_t len = rd32 (e + 24);
      grub_uint32_t bits = rd32 (e + 28);

      if (guid_eq (e, 0x2dc27766, 0xf623, 0x4200, BAT_D4))
        {
          if (bat_found)
            return 0;
          bat->file_offset = off;
          bat->length = len;
          bat_found = 1;
          continue;
        }

      if (guid_eq (e, 0x8b7ca206, 0x4790, 0x4b9a, META_D4))
        {
          if (meta_found)
            return 0;
          meta->file_offset = off;
          meta->length = len;
          meta_found = 1;
          continue;
        }

      if (bits & 0x1)
        return 0;
    }

  return bat_found && meta_found;
}

static int
load_region_table (grub_file_t io,
                   struct vhdx_region_entry *bat,
                   struct vhdx_region_entry *meta)
{
  grub_uint8_t *blk = 0;
  int ok = 0;

  blk = grub_malloc (VHDX_HEADER_BLOCK_SIZE);
  if (!blk)
    return 0;

  if (grub_vdisk_read_exact (io, VHDX_REGION_TBL1_OFF, blk, VHDX_HEADER_BLOCK_SIZE)
      && parse_region_table_block (blk, bat, meta))
    ok = 1;

  if (!ok
      && grub_vdisk_read_exact (io, VHDX_REGION_TBL2_OFF, blk, VHDX_HEADER_BLOCK_SIZE)
      && parse_region_table_block (blk, bat, meta))
    ok = 1;

  grub_free (blk);
  return ok;
}

static int
parse_metadata (grub_file_t io, const struct vhdx_region_entry *meta,
                grub_uint32_t *block_size,
                grub_uint32_t *param_bits,
                grub_uint64_t *virtual_size,
                grub_uint32_t *logical_sector_size)
{
  grub_uint8_t *tbl = 0;
  grub_uint16_t cnt;
  grub_uint32_t i;
  struct vhdx_meta_loc file_param = {0, 0, 0};
  struct vhdx_meta_loc vd_size = {0, 0, 0};
  struct vhdx_meta_loc logical_ss = {0, 0, 0};

  static const grub_uint8_t FILE_PARAM_D4[8] =
    {0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b};
  static const grub_uint8_t VSIZE_D4[8] =
    {0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8};
  static const grub_uint8_t LOGSS_D4[8] =
    {0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f};

  (void) param_bits;

  tbl = grub_malloc (VHDX_METADATA_TBL_SIZE);
  if (!tbl)
    return 0;

  if (!grub_vdisk_read_exact (io, meta->file_offset, tbl, VHDX_METADATA_TBL_SIZE))
    goto fail;

  if (grub_memcmp (tbl, VHDX_SIG_METADATA, 8) != 0)
    goto fail;

  cnt = rd16 (tbl + 10);
  if ((grub_uint32_t) cnt * 32U > VHDX_METADATA_TBL_SIZE - 32)
    goto fail;

  for (i = 0; i < cnt; i++)
    {
      const grub_uint8_t *e = tbl + 32 + i * 32;
      grub_uint32_t off = rd32 (e + 16);
      grub_uint32_t len = rd32 (e + 20);
      grub_uint32_t bits = rd32 (e + 24);

      if (guid_eq (e, 0xcaa16737, 0xfa36, 0x4d43, FILE_PARAM_D4))
        {
          file_param.offset = off;
          file_param.length = len;
          file_param.present = 1;
          continue;
        }
      if (guid_eq (e, 0x2FA54224, 0xcd1b, 0x4876, VSIZE_D4))
        {
          vd_size.offset = off;
          vd_size.length = len;
          vd_size.present = 1;
          continue;
        }
      if (guid_eq (e, 0x8141bf1d, 0xa96f, 0x4709, LOGSS_D4))
        {
          logical_ss.offset = off;
          logical_ss.length = len;
          logical_ss.present = 1;
          continue;
        }

      if (bits & 0x1)
        goto fail;
    }

  if (!file_param.present || !vd_size.present || !logical_ss.present)
    goto fail;

  {
    grub_uint8_t tmp[16];

    if (file_param.length < 8)
      goto fail;
    if (!grub_vdisk_read_exact (io, meta->file_offset + file_param.offset, tmp, 8))
      goto fail;
    *block_size = rd32 (tmp + 0);
    *param_bits = rd32 (tmp + 4);

    if (vd_size.length < 8)
      goto fail;
    if (!grub_vdisk_read_exact (io, meta->file_offset + vd_size.offset, tmp, 8))
      goto fail;
    *virtual_size = rd64 (tmp + 0);

    if (logical_ss.length < 4)
      goto fail;
    if (!grub_vdisk_read_exact (io, meta->file_offset + logical_ss.offset, tmp, 4))
      goto fail;
    *logical_sector_size = rd32 (tmp + 0);
  }

  grub_free (tbl);
  return 1;

fail:
  grub_free (tbl);
  return 0;
}

static grub_err_t
load_bat (grub_file_t io, const struct vhdx_region_entry *bat_r,
          struct vhdx_ctx *ctx)
{
  grub_uint32_t i;

  if ((bat_r->length % 8) != 0)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid VHDX BAT length");

  ctx->bat_entries = bat_r->length / 8;
  ctx->bat = grub_malloc (bat_r->length);
  if (!ctx->bat)
    return grub_errno;

  if (!grub_vdisk_read_exact (io, bat_r->file_offset, ctx->bat, bat_r->length))
    {
      grub_free (ctx->bat);
      ctx->bat = 0;
      return grub_error (GRUB_ERR_READ_ERROR, "failed to read VHDX BAT");
    }

  for (i = 0; i < ctx->bat_entries; i++)
    ctx->bat[i] = grub_le_to_cpu64 (ctx->bat[i]);

  return GRUB_ERR_NONE;
}

int
grub_vhdxio_probe (grub_file_t io, enum grub_file_type type)
{
  grub_uint8_t sig[8];

  if (!grub_vdisk_filter_should_open (io, type,
                                      (grub_off_t) (VHDX_REGION_TBL2_OFF
                                                    + VHDX_HEADER_BLOCK_SIZE)))
    return 0;

  if (!grub_vdisk_read_exact (io, VHDX_FILE_ID_OFF, sig, sizeof (sig)))
    return 0;

  return grub_memcmp (sig, VHDX_SIG_FILE, 8) == 0;
}

grub_file_t
grub_vhdxio_open_filter (grub_file_t io, enum grub_file_type type)
{
  grub_file_t file = 0;
  grub_vhdxio_t vhdxio = 0;
  struct vhdx_region_entry bat = {0, 0};
  struct vhdx_region_entry meta = {0, 0};
  grub_uint8_t hdr[VHDX_HEADER_SIZE];
  grub_uint32_t param_bits = 0;
  grub_uint64_t virtual_size = 0;
  grub_uint32_t block_size = 0;
  grub_uint32_t logical_sector_size = 0;
  grub_uint32_t chunk_ratio = 0;

  if (!grub_vhdxio_probe (io, type))
    return 0;

  if (!load_current_header (io, hdr))
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "no valid VHDX header");
      return 0;
    }

  if (!load_region_table (io, &bat, &meta))
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "no valid VHDX region table");
      return 0;
    }

  if (!parse_metadata (io, &meta, &block_size, &param_bits,
                       &virtual_size, &logical_sector_size))
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid VHDX metadata");
      return 0;
    }

  if (param_bits & VHDX_PARAM_HAS_PARENT)
    {
      grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                  "VHDX differencing disks are unsupported");
      return 0;
    }

  if (logical_sector_size != 512
      && logical_sector_size != 4096)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE,
                  "unsupported VHDX logical sector size");
      return 0;
    }

  if (block_size == 0
      || block_size > (256U * 1024U * 1024U)
      || (block_size & (block_size - 1)) != 0)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid VHDX block size");
      return 0;
    }

  if (block_size < logical_sector_size)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid VHDX geometry");
      return 0;
    }

  chunk_ratio = (VHDX_MAX_SECTORS_PER_BLOCK * logical_sector_size) / block_size;
  if (chunk_ratio == 0 || (chunk_ratio & (chunk_ratio - 1)) != 0)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid VHDX chunk ratio");
      return 0;
    }

  file = grub_vdisk_open (sizeof (*vhdxio), (struct grub_vdisk **) &vhdxio,
                          io, virtual_size,
                          grub_log2ull (logical_sector_size),
                          grub_vhdxio_read, grub_vhdxio_destroy, "vhdx");
  if (!file)
    return 0;

  vhdxio->ctx.file = io;
  vhdxio->ctx.virtual_size = virtual_size;
  vhdxio->ctx.block_size = block_size;
  vhdxio->ctx.logical_sector_size = logical_sector_size;
  vhdxio->ctx.chunk_ratio = chunk_ratio;

  if (load_bat (io, &bat, &vhdxio->ctx) != GRUB_ERR_NONE)
    goto fail;

  grub_dprintf ("vhdxdbg",
                "vhdx: virtual_size=%llu block_size=%u logical_sector=%u bat_entries=%u\n",
                (unsigned long long) vhdxio->ctx.virtual_size,
                vhdxio->ctx.block_size,
                vhdxio->ctx.logical_sector_size,
                vhdxio->ctx.bat_entries);

  return file;

fail:
  grub_vdisk_fail (file, vhdxio ? &vhdxio->disk : 0);
  return 0;
}

static grub_ssize_t
grub_vhdxio_read (struct grub_vdisk *disk, grub_off_t off,
                  char *buf, grub_size_t len)
{
  grub_vhdxio_t vhdxio = (grub_vhdxio_t) disk;
  struct vhdx_ctx *ctx = &vhdxio->ctx;
  grub_size_t total = 0;

  if (off >= (grub_off_t) ctx->virtual_size)
    return 0;
  if (off + len > (grub_off_t) ctx->virtual_size)
    len = ctx->virtual_size - off;

  while (len > 0)
    {
      grub_uint64_t cur = off;
      grub_uint64_t block_idx = cur / ctx->block_size;
      grub_uint32_t within = cur % ctx->block_size;
      grub_uint32_t can = ctx->block_size - within;
      grub_uint64_t bat_idx = block_idx + (block_idx / ctx->chunk_ratio);
      grub_uint64_t entry;
      grub_uint64_t payload_off;
      grub_ssize_t got;
      grub_uint32_t state;

      if (can > len)
        can = len;

      if (bat_idx >= ctx->bat_entries)
        return grub_error (GRUB_ERR_READ_ERROR, "VHDX BAT index out of range");

      entry = ctx->bat[bat_idx];
      state = entry & VHDX_BAT_STATE_MASK;

      switch (state)
        {
        case VHDX_PAYLOAD_NOT_PRESENT:
        case VHDX_PAYLOAD_UNDEFINED:
        case VHDX_PAYLOAD_ZERO:
        case VHDX_PAYLOAD_UNMAPPED:
        case VHDX_PAYLOAD_UNMAPPED_V095:
        case VHDX_PAYLOAD_UNMAPPED_V095_2:
          grub_memset (buf, 0, can);
          break;

        case VHDX_PAYLOAD_FULL:
          payload_off = (entry & VHDX_BAT_FILE_OFF_MASK) + within;
          if (payload_off == 0)
            {
              grub_memset (buf, 0, can);
              break;
            }
          grub_file_seek (ctx->file, payload_off);
          got = grub_file_read (ctx->file, buf, can);
          if (got < 0)
            return got;
          if ((grub_size_t) got != can)
            {
              if (got > 0)
                grub_memset (buf + got, 0, can - got);
              else
                grub_memset (buf, 0, can);
            }
          break;

        case VHDX_PAYLOAD_PARTIAL:
          return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                             "VHDX partial payload blocks are unsupported");

        default:
          return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                             "invalid VHDX BAT payload state");
        }

      buf += can;
      off += can;
      len -= can;
      total += can;
    }

  return total;
}
