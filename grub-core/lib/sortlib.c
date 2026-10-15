/* sortlib.c - sorting and comparison routines. */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2019  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/lib/sortlib.h>

GRUB_MOD_LICENSE ("GPLv3+");

static grub_uint8_t *sort_base;
static grub_size_t sort_width;
static grub_sort_compare_t sort_compare_fn;
static grub_uint8_t *sort_tmp;

static void
swap_elements (grub_size_t a, grub_size_t b)
{
  grub_size_t i;
  grub_uint8_t *left = sort_base + a * sort_width;
  grub_uint8_t *right = sort_base + b * sort_width;

  if (a == b)
    return;
  for (i = 0; i < sort_width; i++)
    {
      grub_uint8_t byte = left[i];
      left[i] = right[i];
      right[i] = byte;
    }
}

static void
sift_down (grub_size_t root, grub_size_t end)
{
  while (root < end / 2)
    {
      grub_size_t child = root * 2 + 1;
      if (child + 1 < end
          && sort_compare_fn (sort_base + child * sort_width,
                              sort_base + (child + 1) * sort_width) < 0)
	child++;
      if (sort_compare_fn (sort_base + root * sort_width,
                           sort_base + child * sort_width) >= 0)
	return;
      swap_elements (root, child);
      root = child;
    }
}

static void
heap_sort (grub_size_t count)
{
  grub_size_t start = count / 2;
  grub_size_t end = count;

  while (start > 0)
    sift_down (--start, count);
  while (end > 1)
    {
      swap_elements (0, --end);
      sift_down (0, end);
    }
}

static grub_size_t
partition (grub_size_t low, grub_size_t high)
{
  grub_size_t pivot_index = low + (high - low) / 2;
  grub_size_t i;
  grub_size_t store = low;

  grub_memcpy (sort_tmp, sort_base + pivot_index * sort_width, sort_width);
  swap_elements (pivot_index, high);
  for (i = low; i < high; i++)
    if (sort_compare_fn (sort_base + i * sort_width, sort_tmp) < 0)
      swap_elements (i, store++);
  swap_elements (store, high);
  return store;
}

static void
intro_sort (grub_size_t low, grub_size_t high, grub_size_t depth)
{
  while (high > low)
    {
      grub_size_t pivot;

      if (depth == 0)
	{
	  /* Sort this subrange with heapsort, retaining the O(n log n) bound. */
	  grub_uint8_t *old_base = sort_base;
	  sort_base += low * sort_width;
	  heap_sort (high - low + 1);
	  sort_base = old_base;
	  return;
	}
      depth--;
      pivot = partition (low, high);

      /* Recurse into the smaller side to keep stack use logarithmic. */
      if (pivot - low < high - pivot)
	{
	  if (pivot > low)
	    intro_sort (low, pivot - 1, depth);
	  low = pivot + 1;
	}
      else
	{
	  if (pivot < high)
	    intro_sort (pivot + 1, high, depth);
	  if (pivot == 0)
	    return;
	  high = pivot - 1;
	}
    }
}

void
perform_quick_sort (void *buf_to_sort, const grub_size_t count,
		    const grub_size_t element_size,
		    sort_compare compare_function)
{
  grub_size_t n, depth = 0;
  grub_size_t max_count = ~(grub_size_t) 0;

  if (!buf_to_sort || !compare_function || count < 2 || element_size == 0
      || count > max_count / element_size)
    return;

  sort_base = buf_to_sort;
  sort_width = element_size;
  sort_compare_fn = compare_function;
  sort_tmp = grub_malloc (element_size);
  if (!sort_tmp)
    return;

  for (n = count; n > 1; n >>= 1)
    depth += 2;
  intro_sort (0, count - 1, depth);

  grub_free (sort_tmp);
  sort_tmp = NULL;
  sort_base = NULL;
  sort_compare_fn = NULL;
}

grub_ssize_t
string_compare (const void *buf1, const void *buf2)
{
  return grub_strcmp (buf1, buf2);
}

void
grub_perform_quick_sort (void *buf_to_sort, grub_size_t count,
			 grub_size_t element_size,
			 grub_sort_compare_t compare_function)
{
  perform_quick_sort (buf_to_sort, count, element_size, compare_function);
}

grub_ssize_t
grub_string_compare (const void *buf1, const void *buf2)
{
  return string_compare (buf1, buf2);
}
