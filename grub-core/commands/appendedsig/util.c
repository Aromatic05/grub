/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2020, 2021, 2022 Free Software Foundation, Inc.
 *  Copyright (C) 2020, 2021, 2022, 2025 IBM Corporation
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/types.h>
#include <grub/mm.h>
#include <grub/misc.h>

#include "util.h"

/*
 * Convert raw binary buffers into lowercase, null-terminated hexadecimal
 * string representations.
 */
grub_err_t
grub_util_buffer2hex (const grub_uint8_t *data, const grub_int32_t data_len,
                      grub_uint8_t **hex, grub_int32_t *hex_len)
{
  static const char hex_digits[] = "0123456789abcdef";
  grub_uint8_t *hex_buf;
  grub_int32_t i;

  if (data == NULL || data_len <= 0 || hex == NULL || hex_len == NULL)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "bad input data");

  if (data_len > (GRUB_INT32_MAX - 1) / 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "data length too large");

  hex_buf = grub_zalloc (data_len * 2 + 1);
  if (hex_buf == NULL)
    return grub_error (GRUB_ERR_OUT_OF_MEMORY, "could not allocate memory");

  for (i = 0; i < data_len; i++)
    {
      hex_buf[i * 2]     = hex_digits[(data[i] >> 4) & 0x0F];
      hex_buf[i * 2 + 1] = hex_digits[data[i] & 0x0F];
    }

  hex_buf[data_len * 2] = '\0';
  *hex = hex_buf;
  *hex_len = data_len * 2;

  return GRUB_ERR_NONE;
}

/*
 * We cannot use hexdump() to display hash data because it is typically displayed
 * in hexadecimal format, along with an ASCII representation of the same data.
 *
 * Example: sha256 hash data
 * 00000000  52 b5 90 49 64 de 22 d7  4e 5f 4f b4 1b 51 9c 34  |R..Id.".N_O..Q.4|
 * 00000010  b1 96 21 7c 91 78 a5 0d  20 8c e9 5c 22 54 53 f7  |..!|.x.. ..\"TS.|
 *
 * An appended signature only required to display the hexadecimal of the hash data
 * by separating each byte with ":". So, we introduced a new method hexdump_colon
 * to display it.
 *
 * Example: Sha256 hash data
 *  52:b5:90:49:64:de:22:d7:4e:5f:4f:b4:1b:51:9c:34:
 *  b1:96:21:7c:91:78:a5:0d:20:8c:e9:5c:22:54:53:f7
 */
void
grub_util_hexdump_colon (const grub_uint8_t *data, const grub_size_t length)
{
  grub_size_t i, count = 0;

  if (data == NULL || length == 0)
    return;

  for (i = 0; i < length - 1; i++)
    {
      grub_printf ("%02x:", data[i]);
      count++;
      if (count == 16)
        {
          grub_printf ("\n         ");
          count = 0;
        }
    }

  grub_printf ("%02x\n", data[i]);
}
