/* sortlib.h - sorting and comparison routines. */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2019  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef GRUB_LIB_SORTLIB_H
#define GRUB_LIB_SORTLIB_H 1

#include <grub/types.h>

typedef grub_ssize_t (*grub_sort_compare_t) (const void *buf1,
						     const void *buf2);

void grub_perform_quick_sort (void *buf_to_sort, grub_size_t count,
			      grub_size_t element_size,
			      grub_sort_compare_t compare_function);

grub_ssize_t grub_string_compare (const void *buf1, const void *buf2);

/* Kept for source and C ABI compatibility with the historic a1ive module. */
typedef grub_sort_compare_t sort_compare;
void perform_quick_sort (void *buf_to_sort, const grub_size_t count,
			 const grub_size_t element_size,
			 sort_compare compare_function);
grub_ssize_t string_compare (const void *buf1, const void *buf2);

#endif /* GRUB_LIB_SORTLIB_H */
