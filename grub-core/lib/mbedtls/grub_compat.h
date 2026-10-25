/*
 * GRUB's libc compatibility surface for vendored Mbed TLS sources.
 * Included by the TLS module build, not by consumers of the library.
 */
#ifndef GRUB_MBEDTLS_COMPAT_H
#define GRUB_MBEDTLS_COMPAT_H

#include <grub/types.h>
#include <grub/mm.h>
#include <grub/misc.h>

#define memcpy grub_memcpy
#define memmove grub_memmove
#define memset grub_memset
#define memcmp grub_memcmp
#define strlen grub_strlen
#define strcmp grub_strcmp
#define strncmp grub_strncmp
#define strchr grub_strchr
#define strstr grub_strstr
#define strncpy grub_strncpy
#define strnlen grub_strnlen
#define snprintf grub_snprintf
#define vsnprintf grub_vsnprintf
#define calloc grub_calloc
#define malloc grub_malloc
#define free grub_free

#endif
