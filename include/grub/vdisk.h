#ifndef GRUB_VDISK_HEADER
#define GRUB_VDISK_HEADER 1

#include <grub/file.h>

struct grub_fs;

struct grub_vdisk;

typedef int (*grub_vdisk_probe_t) (grub_file_t io,
                                   enum grub_file_type type);
typedef grub_file_t (*grub_vdisk_open_t) (grub_file_t io,
                                          enum grub_file_type type);
typedef grub_ssize_t (*grub_vdisk_read_t) (struct grub_vdisk *disk,
                                           grub_off_t off,
                                           char *buf,
                                           grub_size_t len);
typedef void (*grub_vdisk_destroy_t) (struct grub_vdisk *disk);

struct grub_vdisk_parser_desc
{
  const char *name;
  grub_vdisk_probe_t probe;
  grub_vdisk_open_t open;
};

struct grub_vdisk
{
  grub_file_t backing;
  grub_off_t size;
  grub_uint32_t log_sector_size;
  grub_vdisk_read_t read;
  grub_vdisk_destroy_t destroy;
  const char *name;
  /* The parser owns the backing file only after a successful open. */
  int owns_backing;
};

int EXPORT_FUNC(grub_vdisk_filter_should_open) (grub_file_t io,
                                                enum grub_file_type type,
                                                grub_off_t min_size);
int EXPORT_FUNC(grub_vdisk_read_exact) (grub_file_t file,
                                        grub_off_t off,
                                        void *buf,
                                        grub_size_t len);
grub_file_t EXPORT_FUNC(grub_vdisk_create) (grub_size_t object_size,
                                            struct grub_vdisk **disk_out);
grub_file_t EXPORT_FUNC(grub_vdisk_open) (grub_size_t object_size,
                                          struct grub_vdisk **disk_out,
                                          grub_file_t backing,
                                          grub_off_t size,
                                          grub_uint32_t log_sector_size,
                                          grub_vdisk_read_t read,
                                          grub_vdisk_destroy_t destroy,
                                          const char *name);
void EXPORT_FUNC(grub_vdisk_fail) (grub_file_t file,
                                   struct grub_vdisk *disk);
void EXPORT_FUNC(grub_vdisk_attach) (grub_file_t file,
                                     grub_file_t backing,
                                     void *data,
                                     struct grub_fs *fs,
                                     grub_off_t size,
                                     grub_uint32_t log_sector_size);
void EXPORT_FUNC(grub_vdisk_init) (struct grub_vdisk *disk,
                                   grub_file_t backing,
                                   grub_off_t size,
                                   grub_uint32_t log_sector_size,
                                   grub_vdisk_read_t read,
                                   grub_vdisk_destroy_t destroy,
                                   const char *name);
void EXPORT_FUNC(grub_vdisk_attach_object) (grub_file_t file,
                                            struct grub_vdisk *disk);

#endif
