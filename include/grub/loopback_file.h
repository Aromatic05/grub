#ifndef GRUB_LOOPBACK_FILE_HEADER
#define GRUB_LOOPBACK_FILE_HEADER 1

#include <grub/file.h>

/* OWNED_MEMORY is set only when -m allocates a new backing buffer.
   An explicitly supplied mem: buffer always remains caller-owned. */
grub_file_t grub_loopback_file_open (const char *name, int mem, int blocklist,
                                     enum grub_file_type type,
                                     void **owned_memory);
grub_err_t grub_loopback_file_write (grub_file_t file, const void *buf,
                                     grub_size_t len, grub_off_t offset);

#endif
