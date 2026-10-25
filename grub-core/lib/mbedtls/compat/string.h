#ifndef GRUB_MBEDTLS_STRING_H
#define GRUB_MBEDTLS_STRING_H
#include <grub/misc.h>
#define memcpy grub_memcpy
#define memmove grub_memmove
#define memset grub_memset
#define memcmp grub_memcmp
#define memchr grub_memchr
#define strlen grub_strlen
#define strnlen grub_strnlen
#define strcmp grub_strcmp
#define strncmp grub_strncmp
#define strchr grub_strchr
#define strrchr grub_strrchr
#define strstr grub_strstr
#define strcpy grub_strcpy
#define strncpy grub_strncpy
#define strcat grub_strcat
#endif
