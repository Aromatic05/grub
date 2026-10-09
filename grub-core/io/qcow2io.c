/*
 *  GRUB  --  GRand Unified Bootloader
 *
 *  Read-only QCOW2 parser (basic image, no backing chain, no compression).
 */

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/vdisk.h>

GRUB_MOD_LICENSE ("GPLv3+");

int grub_qcow2io_probe (grub_file_t io, enum grub_file_type type);
grub_file_t grub_qcow2io_open_filter (grub_file_t io, enum grub_file_type type);

#define QCOW2_MAGIC                    0x514649fbU

#define QCOW2_OFLAG_COMPRESSED         (1ULL << 62)
#define QCOW2_OFLAG_ZERO               (1ULL << 0)

struct qcow2_header
{
  grub_uint32_t magic;
  grub_uint32_t version;
  grub_uint64_t backing_file_offset;
  grub_uint32_t backing_file_size;
  grub_uint32_t cluster_bits;
  grub_uint64_t size;
  grub_uint32_t crypt_method;
  grub_uint32_t l1_size;
  grub_uint64_t l1_table_offset;
  grub_uint64_t refcount_table_offset;
  grub_uint32_t refcount_table_clusters;
  grub_uint32_t nb_snapshots;
  grub_uint64_t snapshots_offset;
  grub_uint64_t incompatible_features;
  grub_uint64_t compatible_features;
  grub_uint64_t autoclear_features;
  grub_uint32_t refcount_order;
  grub_uint32_t header_length;
};

struct qcow2_ctx
{
  grub_file_t file;

  grub_uint64_t virtual_size;
  grub_uint32_t cluster_bits;
  grub_uint64_t cluster_size;
  grub_uint64_t cluster_mask;

  grub_uint32_t l1_size;
  grub_uint64_t l2_size;
  grub_uint64_t l2_mask;

  grub_uint64_t *l1;

  grub_uint64_t *l2_cache;
  grub_uint64_t l2_cache_off;
  int l2_cache_valid;
};

struct grub_qcow2io
{
  struct grub_vdisk disk;
  struct qcow2_ctx ctx;
};

typedef struct grub_qcow2io *grub_qcow2io_t;

static grub_uint32_t
rd_be32 (const void *p)
{
  grub_uint32_t v;
  grub_memcpy (&v, p, sizeof (v));
  return grub_be_to_cpu32 (v);
}

static grub_uint64_t
rd_be64 (const void *p)
{
  grub_uint64_t v;
  grub_memcpy (&v, p, sizeof (v));
  return grub_be_to_cpu64 (v);
}

static grub_ssize_t
grub_qcow2io_read (struct grub_vdisk *disk, grub_off_t off,
                   char *buf, grub_size_t len);

static void
grub_qcow2io_destroy (struct grub_vdisk *disk)
{
  grub_qcow2io_t qc = (grub_qcow2io_t) disk;

  grub_free (qc->ctx.l2_cache);
  grub_free (qc->ctx.l1);
  if (disk->owns_backing && qc->ctx.file)
    grub_file_close (qc->ctx.file);
  grub_free (qc);
}

static int
qcow2_load_l2 (struct qcow2_ctx *ctx, grub_uint64_t l2_off)
{
  grub_size_t bytes;
  grub_uint64_t i;

  if (ctx->l2_cache_valid && ctx->l2_cache_off == l2_off)
    return 1;

  bytes = (grub_size_t) (ctx->l2_size * sizeof (grub_uint64_t));
  if (l2_off + bytes > (grub_uint64_t) ctx->file->size)
    return 0;

  if (!grub_vdisk_read_exact (ctx->file, l2_off, ctx->l2_cache, bytes))
    return 0;

  for (i = 0; i < ctx->l2_size; i++)
    ctx->l2_cache[i] = grub_be_to_cpu64 (ctx->l2_cache[i]);

  ctx->l2_cache_off = l2_off;
  ctx->l2_cache_valid = 1;
  return 1;
}

int
grub_qcow2io_probe (grub_file_t io, enum grub_file_type type)
{
  grub_uint8_t hraw[8];
  grub_uint32_t magic;
  grub_uint32_t version;

  if (!grub_vdisk_filter_should_open (io, type, (grub_off_t) sizeof (hraw)))
    return 0;

  if (!grub_vdisk_read_exact (io, 0, hraw, sizeof (hraw)))
    return 0;

  magic = rd_be32 (hraw + 0);
  version = rd_be32 (hraw + 4);
  return (magic == QCOW2_MAGIC && version >= 2 && version <= 3);
}

grub_file_t
grub_qcow2io_open_filter (grub_file_t io, enum grub_file_type type)
{
  grub_uint8_t hraw[104];
  struct qcow2_header h;
  grub_file_t file = 0;
  grub_qcow2io_t qc = 0;
  struct qcow2_ctx *ctx;
  grub_uint64_t i;
  grub_size_t l1_bytes;

  if (!grub_qcow2io_probe (io, type))
    return 0;

  if (!grub_vdisk_read_exact (io, 0, hraw, sizeof (hraw)))
    return 0;

  h.magic = rd_be32 (hraw + 0);
  h.version = rd_be32 (hraw + 4);
  h.backing_file_offset = rd_be64 (hraw + 8);
  h.backing_file_size = rd_be32 (hraw + 16);
  h.cluster_bits = rd_be32 (hraw + 20);
  h.size = rd_be64 (hraw + 24);
  h.crypt_method = rd_be32 (hraw + 32);
  h.l1_size = rd_be32 (hraw + 36);
  h.l1_table_offset = rd_be64 (hraw + 40);
  h.refcount_table_offset = rd_be64 (hraw + 48);
  h.refcount_table_clusters = rd_be32 (hraw + 56);
  h.nb_snapshots = rd_be32 (hraw + 60);
  h.snapshots_offset = rd_be64 (hraw + 64);
  h.incompatible_features = rd_be64 (hraw + 72);
  h.compatible_features = rd_be64 (hraw + 80);
  h.autoclear_features = rd_be64 (hraw + 88);
  h.refcount_order = rd_be32 (hraw + 96);
  h.header_length = rd_be32 (hraw + 100);

  if (h.version < 2 || h.version > 3)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "unsupported QCOW2 version");
      return 0;
    }

  if (h.backing_file_size != 0)
    {
      grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                  "QCOW2 backing chain is unsupported");
      return 0;
    }

  if (h.crypt_method != 0)
    {
      grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                  "encrypted QCOW2 is unsupported");
      return 0;
    }

  if (h.cluster_bits < 9 || h.cluster_bits > 21)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid QCOW2 cluster bits");
      return 0;
    }

  if (h.l1_size == 0 || h.size == 0)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid QCOW2 header");
      return 0;
    }

  file = grub_vdisk_open (sizeof (*qc), (struct grub_vdisk **) &qc,
                          io, h.size, GRUB_DISK_SECTOR_BITS,
                          grub_qcow2io_read, grub_qcow2io_destroy, "qcow2");
  if (!file)
    return 0;

  ctx = &qc->ctx;
  ctx->file = io;
  ctx->virtual_size = h.size;
  ctx->cluster_bits = h.cluster_bits;
  ctx->cluster_size = 1ULL << h.cluster_bits;
  ctx->cluster_mask = ctx->cluster_size - 1;

  ctx->l2_size = 1ULL << (h.cluster_bits - 3);
  ctx->l2_mask = ctx->l2_size - 1;
  ctx->l1_size = h.l1_size;

  l1_bytes = (grub_size_t) ((grub_uint64_t) ctx->l1_size * sizeof (grub_uint64_t));
  if (h.l1_table_offset + l1_bytes > (grub_uint64_t) io->size)
    goto fail;

  ctx->l1 = grub_malloc (l1_bytes);
  if (!ctx->l1)
    goto fail;

  if (!grub_vdisk_read_exact (io, h.l1_table_offset, ctx->l1, l1_bytes))
    goto fail;

  for (i = 0; i < ctx->l1_size; i++)
    ctx->l1[i] = grub_be_to_cpu64 (ctx->l1[i]);

  ctx->l2_cache = grub_malloc ((grub_size_t) (ctx->l2_size * sizeof (grub_uint64_t)));
  if (!ctx->l2_cache)
    goto fail;
  ctx->l2_cache_valid = 0;
  ctx->l2_cache_off = 0;

  return file;

fail:
  grub_vdisk_fail (file, qc ? &qc->disk : 0);
  return 0;
}

static grub_ssize_t
grub_qcow2io_read (struct grub_vdisk *disk, grub_off_t off,
                   char *buf, grub_size_t len)
{
  grub_qcow2io_t qc = (grub_qcow2io_t) disk;
  struct qcow2_ctx *ctx = &qc->ctx;
  grub_size_t done = 0;

  if (off >= (grub_off_t) disk->size)
    return 0;
  if (off + len > (grub_off_t) disk->size)
    len = disk->size - off;

  while (len > 0)
    {
      grub_uint64_t cur = off;
      grub_uint64_t l1_index = cur >> (ctx->cluster_bits + (ctx->cluster_bits - 3));
      grub_uint64_t l2_index = (cur >> ctx->cluster_bits) & ctx->l2_mask;
      grub_uint64_t in_cluster = cur & ctx->cluster_mask;
      grub_uint32_t chunk = (ctx->cluster_size - in_cluster > len)
                            ? (grub_uint32_t) len
                            : (grub_uint32_t) (ctx->cluster_size - in_cluster);
      grub_uint64_t l1e;
      grub_uint64_t l2_off;
      grub_uint64_t l2e;
      grub_uint64_t data_off;

      if (l1_index >= ctx->l1_size)
        return grub_error (GRUB_ERR_READ_ERROR, "QCOW2 L1 index out of range");

      l1e = ctx->l1[l1_index];
      l2_off = l1e & ~ctx->cluster_mask;

      if (l2_off == 0)
        {
          grub_memset (buf, 0, chunk);
          goto next;
        }

      if (!qcow2_load_l2 (ctx, l2_off))
        return grub_error (GRUB_ERR_READ_ERROR, "failed to load QCOW2 L2 table");

      l2e = ctx->l2_cache[l2_index];

      if (l2e == 0 || ((l2e & QCOW2_OFLAG_ZERO) && ((l2e & ~ctx->cluster_mask) == 0)))
        {
          grub_memset (buf, 0, chunk);
          goto next;
        }

      if (l2e & QCOW2_OFLAG_COMPRESSED)
        return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                           "compressed QCOW2 clusters are unsupported");

      data_off = (l2e & ~ctx->cluster_mask) + in_cluster;
      if (data_off + chunk > (grub_uint64_t) ctx->file->size)
        return grub_error (GRUB_ERR_READ_ERROR, "QCOW2 data cluster points outside file");

      grub_file_seek (ctx->file, data_off);
      if (grub_file_read (ctx->file, buf, chunk) != (grub_ssize_t) chunk)
        return grub_error (GRUB_ERR_READ_ERROR, "failed to read QCOW2 cluster data");

next:
      buf += chunk;
      off += chunk;
      len -= chunk;
      done += chunk;
    }

  return done;
}
