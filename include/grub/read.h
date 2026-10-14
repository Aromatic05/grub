/* Shared line input for the GRUB read command and Lua input.read(). */
#ifndef GRUB_READ_HEADER
#define GRUB_READ_HEADER 1

#include <grub/types.h>

/* 0: echo characters; 1: echo '*'; 2: no echo.  Caller frees the line. */
char *EXPORT_FUNC (grub_getline) (int hide_mode);

#endif
