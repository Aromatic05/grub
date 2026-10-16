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
 *
 *  EFI TPCM support code.
 */

#include <grub/err.h>
#include <grub/efi/tpcm.h>
#include <grub/misc.h>


static grub_uint32_t g_measured_id = STAGE_START;

/*
 * get_tpcm_stage:
 * TPCM does not make a distinction with the type of
 * measured target, so we use g_measured_id directly
 * for the stage.
 */
static grub_uint32_t
get_tpcm_stage (void)
{
    grub_uint32_t stage = STAGE_INVALID;

    stage = g_measured_id;

    if (stage < STAGE_START || stage > STAGE_END)
      stage = STAGE_INVALID;

    return stage;
}

/*
 * update_measured_id:
 * update g_measured_id +1 every time measured, and g_measured_id
 * will never be decreased.
 */
static void
update_measured_id (void)
{
    g_measured_id++;
}

/*
 * measure_memory:
 * measure the memory region--(addr, size) through the TPCM protocol.
 * if TPCM protocol is not exist in BIOS, it will return SUCC to keep
 * compatible with non-measurement-support bios; if TPCM protocol is
 * exist but not enabled, it will also return SUCC.
 */
static grub_err_t
measure_memory (enum grub_file_type type __attribute__((unused)),
                char *desc, grub_addr_t addr, grub_size_t size)
{
    grub_efi_handle_t   *handles = NULL;
    grub_efi_uintn_t    num_handles;
    grub_efi_handle_t   grub_c2p_handle = 0;
    grub_err_t  test_c2p_err = GRUB_ERR_BAD_OS;
    grub_guid_t         c2p_guid = GRUB_EFI_TPCM_PROTOCOL_GUID;
    grub_uint32_t       measure_result = 0;
    grub_uint32_t       control_result = 0;
    grub_efi_boolean_t    verify_enable = false;
    grub_size_t         desc_len = 0;

    handles = grub_efi_locate_handle (GRUB_EFI_BY_PROTOCOL, &c2p_guid,
                                      NULL, &num_handles);
    if (handles != NULL && num_handles > 0)
    {
        struct c2p_protocol *c2p;

        grub_c2p_handle = handles[0];
        grub_dprintf ("tpcm", "measure memory addr 0x%zx size 0x%zx\n",
                      addr, size);
        c2p = grub_efi_open_protocol (grub_c2p_handle, &c2p_guid,
                                      GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (c2p != NULL)
        {
            verify_enable = c2p->verify_is_enabled (c2p);
            if (verify_enable == true)
            {
                struct addr_range range;
                grub_efi_status_t status = 0;
                grub_uint32_t stage = STAGE_INVALID;

                range.start  = addr;
                range.length = size;

                stage = get_tpcm_stage ();
                if (stage != STAGE_INVALID)
                {
                    desc_len = grub_strlen (desc) + 1;
                    status = c2p->verify_raw (c2p, stage,
                                              (grub_uint64_t) desc, desc_len,
                                              1, &range, &measure_result,
                                              &control_result);
                    if (status == GRUB_EFI_SUCCESS
                        && (control_result & MEASURE_ACTION_MASK) == 0)
                    {
                        grub_dprintf ("tpcm", "verify_raw success. "
                                      "stage[%d]desc:[%s]\n", stage, desc);
                        test_c2p_err = GRUB_ERR_NONE;
                    }
                    else
                    {
                        grub_free (handles);
                        grub_fatal ("tpcm verify error. stage[%d]desc[%s]",
                                    stage, desc);
                    }
                }
                else
                  grub_dprintf ("tpcm", "invalid stage\n");

                update_measured_id ();

            }
            else
            {
                grub_dprintf ("tpcm", "image verify not enabled\n");
                test_c2p_err = GRUB_ERR_NONE;
            }
        }
        else
            grub_dprintf ("tpcm", "open c2p protocol failed\n");
    }
    else
    {
        /* keep compatible with non-measurement-support bios. */
        grub_dprintf ("tpcm", "not found C2P protocol\n");
        test_c2p_err = GRUB_ERR_NONE;
    }

    if (handles)
      grub_free (handles);

    return test_c2p_err;
}

grub_err_t
grub_tpcm_parse_context (void *context, enum grub_file_type *type,
                         char **description)
{
    char *p_context = (char *) context;
    char *p;
    char tmp[TPCM_MAX_BUF_SIZE] = {'0'};

    if (p_context == NULL)
        return GRUB_ERR_BUG;
    if (type == NULL || description == NULL)
        return GRUB_ERR_BAD_ARGUMENT;

    p = grub_strchr (p_context, '|');
    if (p == NULL || p == p_context
        || (grub_size_t) (p - p_context) >= sizeof (tmp))
        return GRUB_ERR_BAD_ARGUMENT;

    grub_memcpy (tmp, p_context, (p - p_context));
    *type = grub_strtoul (tmp, 0, 10);
    *description = p + 1;

    return GRUB_ERR_NONE;
}

/* grub_tpcm_measure_memory */
grub_err_t
grub_tpcm_measure_memory (void *context, grub_addr_t buf, grub_size_t size)
{
    char *p_desc;
    enum grub_file_type type;
    grub_err_t err;

    err = grub_tpcm_parse_context (context, &type, &p_desc);
    if (err != GRUB_ERR_NONE)
        return err;

    return measure_memory (type, p_desc, buf, size);
}
