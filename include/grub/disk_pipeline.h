/* Ordered construction stages for file-backed virtual disks. */
#ifndef GRUB_DISK_PIPELINE_HEADER
#define GRUB_DISK_PIPELINE_HEADER 1

#include <grub/dl.h>
#include <grub/file.h>

enum grub_disk_stage
  {
    GRUB_DISK_SOURCE,
    GRUB_DISK_VIEW,
    GRUB_DISK_FORMAT,
    GRUB_DISK_BLOCK,
    GRUB_DISK_PUBLISH,
    GRUB_DISK_STAGE_COUNT
  };

enum grub_disk_pipeline_flag
  {
    GRUB_DISK_PIPELINE_MEMORY = 1 << 0,
    GRUB_DISK_PIPELINE_BLOCKLIST = 1 << 1,
    GRUB_DISK_PIPELINE_DECOMPRESS = 1 << 2,
    GRUB_DISK_PIPELINE_FORMAT = 1 << 3,
    GRUB_DISK_PIPELINE_COW = 1 << 4
  };

enum grub_disk_target
  {
    GRUB_DISK_TARGET_LOOPBACK,
    GRUB_DISK_TARGET_EFI
  };

/* Prepared block reader/writer chain. A BLOCK hook may prepend a layer.
   Each layer delegates to lower or implements its own backing policy. */
struct grub_disk_layer
{
  struct grub_disk_layer *lower;
  void *data;
  grub_err_t (*read) (struct grub_disk_layer *, grub_uint64_t,
                      grub_size_t, void *);
  grub_err_t (*write) (struct grub_disk_layer *, grub_uint64_t,
                       grub_size_t, const void *);
  void (*close) (struct grub_disk_layer *);
};

struct grub_disk_pipeline
{
  char *source;
  char *name;
  enum grub_disk_target target;
  unsigned int flags;
  enum grub_file_type type;
  grub_file_t file;
  void *owned_memory;
  grub_err_t (*direct_write) (grub_file_t, const void *, grub_size_t, grub_off_t);
  grub_dl_t format_module;
  struct grub_disk_layer base;
  struct grub_disk_layer *io;
  int transformed;
  int formatted;
  int published;
};

struct grub_disk_hook
{
  const char *name;
  enum grub_disk_stage stage;
  int order;
  int (*probe) (struct grub_disk_pipeline *, const struct grub_disk_hook *);
  grub_err_t (*run) (struct grub_disk_pipeline *, const struct grub_disk_hook *);
  const void *data;
  grub_dl_t module;
  struct grub_disk_hook *next;
};

grub_err_t EXPORT_FUNC (grub_disk_hook_register) (struct grub_disk_hook *hook,
                                                   grub_dl_t module);
void EXPORT_FUNC (grub_disk_hook_unregister) (struct grub_disk_hook *hook);
int EXPORT_FUNC (grub_disk_stage_available) (enum grub_disk_stage stage);
/* Apply the same VIEW hook chain used by ordinary grub_file_open(). */
grub_err_t EXPORT_FUNC (grub_disk_pipeline_views)
  (struct grub_disk_pipeline *ctx);

struct grub_disk_pipeline *EXPORT_FUNC (grub_disk_pipeline_new)
  (const char *source, const char *name, enum grub_disk_target target,
   unsigned int flags);
grub_err_t EXPORT_FUNC (grub_disk_pipeline_run) (struct grub_disk_pipeline *ctx);
void EXPORT_FUNC (grub_disk_pipeline_close) (struct grub_disk_pipeline *ctx);

grub_err_t EXPORT_FUNC (grub_disk_pipeline_read)
  (struct grub_disk_pipeline *ctx, grub_uint64_t off, grub_size_t len, void *buf);
grub_err_t EXPORT_FUNC (grub_disk_pipeline_write)
  (struct grub_disk_pipeline *ctx, grub_uint64_t off, grub_size_t len,
   const void *buf);

#endif
