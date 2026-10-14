#ifndef GRUB_I386_MULTIBOOT_INFO_H
#define GRUB_I386_MULTIBOOT_INFO_H 1

#include <multiboot.h>
#include <grub/symbol.h>

const struct multiboot_info *EXPORT_FUNC (grub_i386_multiboot_get_info) (void);

#endif
