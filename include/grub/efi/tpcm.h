/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2025  Free Software Foundation, Inc.
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

#ifndef GRUB_EFI_TPCM_HEADER
#define GRUB_EFI_TPCM_HEADER 1

#include <grub/file.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>

/*
 * TPCM EFI protocol GUID. The firmware protocol is currently provided by
 * TPCM-capable platforms; C2P is the firmware interface used by those
 * implementations to verify memory ranges before boot.
 */
#define GRUB_EFI_TPCM_PROTOCOL_GUID \
  {0xf89ab5cd, 0x2829, 0x422f, \
   {0xa5, 0xf3, 0x03, 0x28, 0xe0, 0x6c, 0xfc, 0xbb}}
#define MEASURE_ACTION_MASK          (0x1)
#define TPCM_MAX_BUF_SIZE            128

/*
 * stage layout:
 * 2000~2999: +1 every time
 */

#define STAGE_START      2000
#define STAGE_END        2999
#define STAGE_INVALID    3000

struct addr_range {
    grub_uint64_t start;
    grub_uint64_t length;
};
struct c2p_protocol {
    grub_efi_status_t (__grub_efi_api *verify_raw) (struct c2p_protocol *this,
                                    grub_uint32_t measure_stage,
                                    grub_uint64_t image_info,
                                    grub_uint32_t image_info_size,
                                    grub_uint32_t num_addr_range,
                                    struct addr_range ranges[],
                                    grub_uint32_t *measure_result,
                                    grub_uint32_t *control_result);
    grub_efi_boolean_t (__grub_efi_api *verify_is_enabled)
                                    (struct c2p_protocol *this);
};

grub_err_t
grub_tpcm_measure_memory (void *context, grub_addr_t buf, grub_size_t size);

grub_err_t
grub_tpcm_parse_context (void *context, enum grub_file_type *type,
			 char **description);

#endif
