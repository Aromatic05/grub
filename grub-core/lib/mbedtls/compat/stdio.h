#ifndef GRUB_MBEDTLS_STDIO_H
#define GRUB_MBEDTLS_STDIO_H
#include <grub/misc.h>
#define EOF (-1)
#define printf grub_printf
#define snprintf grub_snprintf
#define vsnprintf grub_vsnprintf
#endif
