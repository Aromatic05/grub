#ifndef GRUB_MBEDTLS_STDLIB_H
#define GRUB_MBEDTLS_STDLIB_H
#include <grub/mm.h>
#include <grub/misc.h>
#define malloc grub_malloc
#define calloc grub_calloc
#define realloc grub_realloc
#define free grub_free
#define strtol grub_strtol
#define strtoul grub_strtoul
#define abort grub_abort
#endif
