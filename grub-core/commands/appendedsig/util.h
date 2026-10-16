/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2020, 2022 Free Software Foundation, Inc.
 *  Copyright (C) 2020, 2022, 2025 IBM Corporation
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

#ifndef UTIL_H
#define UTIL_H

/*
 * Convert raw binary buffers into lowercase, null-terminated hexadecimal
 * string representations.
 */
extern grub_err_t
grub_util_buffer2hex (const grub_uint8_t *data, const grub_int32_t data_len,
                      grub_uint8_t **hex, grub_int32_t *hex_len);

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
extern void
grub_util_hexdump_colon (const grub_uint8_t *data, const grub_size_t length);

#endif /* UTIL_H */
