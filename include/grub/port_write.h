#ifndef GRUB_PORT_WRITE_HEADER
#define GRUB_PORT_WRITE_HEADER 1

#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/memfile.h>
#include <grub/misc.h>
#include <grub/partition.h>

struct grub_port_fs_block
{
  grub_disk_addr_t offset;
  grub_disk_addr_t length;
};

struct grub_port_blocklist_ctx
{
  int num;
  struct grub_port_fs_block *blocks;
  grub_off_t total_size;
  grub_disk_addr_t part_start;
};

static int
grub_port_is_mem_name (const char *name)
{
  return grub_memfile_is_name (name)
      || (name && grub_strncmp (name, "(mem)", 5) == 0);
}

static grub_ssize_t
grub_port_blocklist_read (grub_file_t file, char *buf, grub_size_t len)
{
  struct grub_port_fs_block *p;
  grub_off_t offset;
  grub_ssize_t ret = 0;
  grub_disk_t disk = file->device ? file->device->disk : 0;

  if (!disk)
    return -1;

  if (len > file->size - file->offset)
    len = file->size - file->offset;

  offset = file->offset;
  disk->read_hook = file->read_hook;
  disk->read_hook_data = file->read_hook_data;
  for (p = file->data; p && p->length && len > 0; p++)
    {
      if (offset < (grub_off_t) p->length)
        {
          grub_size_t size = len;
          grub_disk_addr_t block_offset = p->offset + offset;
          grub_disk_addr_t sector = block_offset >> GRUB_DISK_SECTOR_BITS;
          grub_off_t sec_off = block_offset & (GRUB_DISK_SECTOR_SIZE - 1);

          if (offset + size > (grub_off_t) p->length)
            size = p->length - offset;

          if (grub_disk_read (disk, sector, sec_off, size, buf) != GRUB_ERR_NONE)
            {
              ret = -1;
              break;
            }

          ret += size;
          len -= size;
          buf += size;
          offset += size;
        }
      else
        offset -= p->length;
    }
  disk->read_hook = 0;
  disk->read_hook_data = 0;
  return ret;
}

static grub_err_t
grub_port_blocklist_close (grub_file_t file)
{
  grub_free (file->data);
  file->data = 0;
  return GRUB_ERR_NONE;
}

static struct grub_fs grub_port_blocklist_fs =
{
  .name = "blocklist",
  .fs_dir = 0,
  .fs_open = 0,
  .fs_read = grub_port_blocklist_read,
  .fs_close = grub_port_blocklist_close,
  .next = 0
};

static grub_err_t
grub_port_collect_block (grub_disk_addr_t sector, unsigned offset,
                         unsigned length, char *buf __attribute__ ((unused)),
                         void *ctxp)
{
  struct grub_port_blocklist_ctx *ctx = ctxp;
  grub_off_t block_offset;

  block_offset = ((sector - ctx->part_start) << GRUB_DISK_SECTOR_BITS) + offset;

  if (ctx->num > 0
      && (ctx->blocks[ctx->num - 1].offset + ctx->blocks[ctx->num - 1].length
          == (grub_disk_addr_t) block_offset))
    {
      ctx->blocks[ctx->num - 1].length += length;
      ctx->total_size += length;
      return GRUB_ERR_NONE;
    }

  if ((ctx->num & 7) == 0)
    {
      struct grub_port_fs_block *new_blocks;
      new_blocks = grub_realloc (ctx->blocks,
                                 (ctx->num + 8) * sizeof (*ctx->blocks));
      if (!new_blocks)
        return grub_errno;
      ctx->blocks = new_blocks;
    }

  ctx->blocks[ctx->num].offset = (grub_disk_addr_t) block_offset;
  ctx->blocks[ctx->num].length = length;
  ctx->num++;
  ctx->total_size += length;
  return GRUB_ERR_NONE;
}

static int
grub_port_is_blocklist_file (grub_file_t file)
{
  return file && file->fs && file->fs->name
         && grub_strcmp (file->fs->name, "blocklist") == 0
         && file->device && file->device->disk && file->data;
}

static int
grub_port_file_prepare_write (grub_file_t file)
{
  grub_disk_read_hook_t saved_hook;
  void *saved_hook_data;
  grub_fs_t old_fs;
  grub_off_t saved_offset;
  grub_off_t start_offset;
  grub_off_t expected;
  char buf[4096];
  struct grub_port_blocklist_ctx ctx;

  if (!file)
    return 0;

  if (grub_port_is_mem_name (file->name))
    return (file->data != 0);

  if (grub_port_is_blocklist_file (file))
    return 1;

  if (!file->device || !file->device->disk || !file->fs || !file->data)
    return 0;

  saved_offset = file->offset;
  start_offset = file->offset;
  expected = file->size - start_offset;

  ctx.num = 0;
  ctx.blocks = 0;
  ctx.total_size = 0;
  ctx.part_start = grub_partition_get_start (file->device->disk->partition);

  saved_hook = file->read_hook;
  saved_hook_data = file->read_hook_data;
  file->read_hook = grub_port_collect_block;
  file->read_hook_data = &ctx;

  while (grub_file_read (file, buf, sizeof (buf)) > 0)
    ;

  file->read_hook = saved_hook;
  file->read_hook_data = saved_hook_data;

  if (grub_errno != GRUB_ERR_NONE || ctx.total_size != expected || ctx.num == 0)
    {
      grub_free (ctx.blocks);
      return 0;
    }

  old_fs = file->fs;
  if (old_fs->fs_close)
    old_fs->fs_close (file);
  if (old_fs->mod)
    grub_dl_unref (old_fs->mod);

  file->fs = &grub_port_blocklist_fs;
  file->data = ctx.blocks;
  file->offset = saved_offset;
  return 1;
}

static grub_ssize_t
grub_port_file_write (grub_file_t file, const char *buf, grub_size_t len)
{
  struct grub_port_fs_block *p;
  grub_off_t offset;
  grub_ssize_t ret = 0;
  grub_disk_t disk;

  if (!file || !buf)
    return -1;

  if (file->offset > file->size)
    return -1;

  if ((grub_off_t) len > file->size - file->offset)
    len = (grub_size_t) (file->size - file->offset);

  if (len == 0)
    return 0;

  if (grub_port_is_mem_name (file->name))
    {
      if (!file->data)
        return -1;
      grub_memcpy ((grub_uint8_t *) file->data + file->offset, buf, len);
      return (grub_ssize_t) len;
    }

  if (!grub_port_is_blocklist_file (file) && !grub_port_file_prepare_write (file))
    return -1;

  if (!grub_port_is_blocklist_file (file))
    return -1;

  disk = file->device->disk;
  offset = file->offset;
  for (p = file->data; p && p->length && len > 0; p++)
    {
      if (offset < (grub_off_t) p->length)
        {
          grub_size_t size = len;
          grub_disk_addr_t block_offset = p->offset + offset;
          grub_disk_addr_t sector = block_offset >> GRUB_DISK_SECTOR_BITS;
          grub_off_t sec_off = block_offset & (GRUB_DISK_SECTOR_SIZE - 1);

          if (offset + size > (grub_off_t) p->length)
            size = p->length - offset;

#ifdef GRUB_UTIL
          if (grub_disk_write (disk, sector, sec_off, size, buf) != GRUB_ERR_NONE)
            return -1;
#else
          /* disk.mod is optional. Common SOURCE must never depend on it
             at link time merely because some backing files are writable. */
          if (!grub_disk_write_weak
              || grub_disk_write_weak (disk, sector, sec_off, size, buf)
                   != GRUB_ERR_NONE)
            return -1;
#endif

          ret += size;
          len -= size;
          buf += size;
          offset += size;
        }
      else
        offset -= p->length;
    }

  return ret;
}

#endif
