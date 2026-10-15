/* Physical block ranges for file-backed disk images. */
#ifndef GRUB_DISK_BLOCKMAP_HEADER
#define GRUB_DISK_BLOCKMAP_HEADER 1

#include <grub/types.h>
#include <grub/file.h>

#define GRUB_BLOCKMAP_DEFAULT_CHUNKS 1024
#define GRUB_BLOCKMAP_BUFFER_SIZE (4 * 1024 * 1024)

/* Logical image sectors and physical disk sectors use separate units. */
typedef struct grub_disk_blockmap_chunk
{
  grub_uint32_t img_start_sector;
  grub_uint32_t img_end_sector;
  grub_uint64_t disk_start_sector;
  grub_uint64_t disk_end_sector;
} grub_disk_blockmap_chunk;

typedef struct grub_disk_blockmap
{
  grub_uint32_t max_chunk;
  grub_uint32_t cur_chunk;
  grub_uint32_t err_code;
  grub_uint32_t last_offset;
  grub_disk_blockmap_chunk *chunk;
  char *buf;
} grub_disk_blockmap;

enum grub_disk_blockmap_error
{
  GRUB_BLOCKMAP_ERR_NONE = 0,
  GRUB_BLOCKMAP_ERR_MULTI_DEV,
  GRUB_BLOCKMAP_ERR_RAID,
  GRUB_BLOCKMAP_ERR_COMPRESS,
  GRUB_BLOCKMAP_ERR_NOT_FLAT,
  GRUB_BLOCKMAP_ERR_OVER_FLOW,
  GRUB_BLOCKMAP_ERR_MAX
};

int EXPORT_FUNC (grub_fat_get_file_chunk) (grub_uint64_t part_start,
                                           grub_file_t file,
                                           grub_disk_blockmap *map);
int EXPORT_FUNC (grub_ext_get_file_chunk) (grub_uint64_t part_start,
                                           grub_file_t file,
                                           grub_disk_blockmap *map);
int EXPORT_FUNC (grub_btrfs_get_file_chunk) (grub_uint64_t part_start,
                                             grub_file_t file,
                                             grub_disk_blockmap *map);
grub_uint64_t EXPORT_FUNC (grub_udf_get_file_offset) (grub_file_t file);
grub_uint64_t EXPORT_FUNC (grub_udf_get_last_pd_size_offset) (void);
grub_uint64_t EXPORT_FUNC (grub_udf_get_last_file_attr_offset)
  (grub_file_t file, grub_uint32_t *startBlock,
   grub_uint64_t *fe_entry_size_offset);

#endif
