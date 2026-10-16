/******************************************************************************
 * ventoy.c 
 *
 * Copyright (c) 2020, longpanda <admin@ventoy.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <grub/types.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/err.h>
#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/device.h>
#include <grub/term.h>
#include <grub/partition.h>
#include <grub/file.h>
#include <grub/fshelp.h>
#include <grub/hashsum.h>
#include <grub/crypto.h>
#include <grub/normal.h>
#include <grub/extcmd.h>
#include <grub/datetime.h>
#include <grub/net.h>
#include <grub/misc.h>
#include <grub/kernel.h>
#include <grub/time.h>
#include <grub/memory.h>
#ifdef GRUB_MACHINE_EFI
#include <grub/efi/efi.h>
#include <grub/efi/memory.h>
#endif
#include <grub/ventoy.h>
#include "ventoy_def.h"
#include "ventoy_compat.h"

GRUB_MOD_LICENSE ("GPLv3+");

int g_ventoy_debug = 0;
static int g_efi_os = 0xFF;
grub_uint32_t g_ventoy_plat_data;

/* The optional Ventoy Shim protocol is a Ventoy-local EFI integration, not
   a modification of GRUB's ordinary Secure Boot image verification. */
#ifdef GRUB_MACHINE_EFI
typedef struct ventoy_shim_policy
{
    void (__grub_efi_api *ByPassSB)(void);
    void (__grub_efi_api *CheckSB)(void);
    void (__grub_efi_api *Launched)(void);
} ventoy_shim_policy;

static ventoy_shim_policy *g_ventoy_shim;
#endif
static int g_ventoy_system_sb;
static int g_ventoy_sb_policy;
static int g_ventoy_sb_policy_applied;

const char *
ventoy_sb_policy_name(void)
{
    if (g_ventoy_sb_policy == 0)
        return "ByPass";
    if (g_ventoy_sb_policy == 1)
        return "Check";
    return "XXX";
}

int
ventoy_set_sb_policy(void)
{
    const char *policy;

    if (!g_ventoy_system_sb || g_ventoy_sb_policy_applied)
        return 0;

    g_ventoy_sb_policy_applied = 1;
    policy = grub_env_get("VTOY_SECURE_BOOT_POLICY");
    if (policy)
        g_ventoy_sb_policy = (grub_uint8_t) (policy[0] - '0');

#ifdef GRUB_MACHINE_EFI
    if (g_ventoy_shim)
    {
        if (g_ventoy_sb_policy == 0 && g_ventoy_shim->ByPassSB)
            g_ventoy_shim->ByPassSB();
        else if (g_ventoy_sb_policy == 1 && g_ventoy_shim->CheckSB)
            g_ventoy_shim->CheckSB();
    }
#endif
    return 0;
}

#ifdef GRUB_MACHINE_EFI
static void
ventoy_secure_boot_init(void)
{
    static const grub_guid_t shim_guid =
        { 0x90a29d14, 0x3968, 0x48fe,
          { 0x85, 0x81, 0x6b, 0x7f, 0x7d, 0xc4, 0x70, 0x55 } };
    grub_guid_t global = GRUB_EFI_GLOBAL_VARIABLE_GUID;
    grub_uint8_t *setup_mode = NULL;
    grub_size_t size = 0;

    g_ventoy_shim = NULL;
    g_ventoy_system_sb = ventoy_compat_efi_secureboot_enabled();
    if (!g_ventoy_system_sb)
        return;

    if (grub_efi_get_variable("SetupMode", &global, &size,
                              (void **) &setup_mode) == GRUB_EFI_SUCCESS
        && size == 1 && setup_mode && *setup_mode == 1)
        g_ventoy_system_sb = 0;
    grub_free(setup_mode);

    /* Upstream's Shim protocol is published only on the x86_64 EFI path. */
    if (g_ventoy_system_sb && g_ventoy_plat_data == VTOY_PLAT_X86_64_UEFI)
    {
        g_ventoy_shim = grub_efi_locate_protocol((grub_guid_t *) &shim_guid, NULL);
        if (g_ventoy_shim && g_ventoy_shim->Launched
            && g_ventoy_shim->ByPassSB && g_ventoy_shim->CheckSB)
            g_ventoy_shim->Launched();
        else
            g_ventoy_shim = NULL;
    }
}
#endif

void ventoy_debug(const char *fmt, ...)
{
    va_list args;

    va_start (args, fmt);
    grub_vprintf (fmt, args);
    va_end (args);
}

void ventoy_str_tolower(char *str)
{
    while (*str)
    {
        *str = grub_tolower(*str);
        str++;
    }
}

void ventoy_str_toupper(char *str)
{
    while (*str)
    {
        *str = grub_toupper(*str);
        str++;
    }
}

char *ventoy_str_last(char *str, char ch)
{
    char *pos = NULL;
    char *last = NULL;

    if (!str)
    {
        return NULL;
    }

    for (pos = str; *pos; pos++)
    {
        if (*pos == ch)
        {
            last = pos;
        }
    }

    return last;
}

int ventoy_str_all_digit(const char *str)
{
    if (NULL == str || 0 == *str)
    {
        return 0;
    }

    while (*str)
    {
        if (*str < '0' || *str > '9')
        {
            return 0;
        }
    }

    return 1;
}

int ventoy_str_all_alnum(const char *str)
{
    if (NULL == str || 0 == *str)
    {
        return 0;
    }

    while (*str)
    {
        if (!grub_isalnum(*str))
        {
            return 0;
        }
    }

    return 1;
}

int ventoy_str_len_alnum(const char *str, int len)
{
    int i;
    int slen;
    
    if (NULL == str || 0 == *str)
    {
        return 0;
    }

    slen = grub_strlen(str);
    if (slen <= len)
    {
        return 0;
    }

    for (i = 0; i < len; i++)
    {
        if (!grub_isalnum(str[i]))
        {
            return 0;
        }
    }

    if (str[len] == 0 || grub_isspace(str[len]))
    {
        return 1;
    }

    return 0;
}

char * ventoy_str_basename(char *path)
{
    char *pos = NULL;
    
    pos = grub_strrchr(path, '/');
    if (pos)
    {
        pos++;
    }
    else
    {
        pos = path;
    }

    return pos;
}

int ventoy_str_chrcnt(const char *str, char c)
{
    int n = 0;
    
    if (str)
    {
        while (*str)
        {
            if (*str == c)
            {
                n++;                
            }
            str++;
        }        
    }

    return n;
}

int ventoy_strcmp(const char *pattern, const char *str)
{
    while (*pattern && *str)
    {
        if ((*pattern != *str) && (*pattern != '*'))
            break;

        pattern++;
        str++;
    }

    return (int)(grub_uint8_t)*pattern - (int)(grub_uint8_t)*str;
}

int ventoy_strncmp (const char *pattern, const char *str, grub_size_t n)
{
    if (n == 0)
        return 0;

    while (*pattern && *str && --n)
    {
        if ((*pattern != *str) && (*pattern != '*'))
            break;

        pattern++;
        str++;
    }

    return (int)(grub_uint8_t)*pattern - (int)(grub_uint8_t)*str;
}

grub_err_t ventoy_env_int_set(const char *name, int value)
{
    char buf[16];
    
    grub_snprintf(buf, sizeof(buf), "%d", value);
    return grub_env_set(name, buf);
}

void ventoy_debug_dump_guid(const char *prefix, grub_uint8_t *guid)
{
    int i;

    if (!g_ventoy_debug)
    {
        return;
    }
    
    debug("%s", prefix);
    for (i = 0; i < 16; i++)
    {
        grub_printf("%02x ", guid[i]);
    }
    grub_printf("\n");       
}

int ventoy_is_efi_os(void)
{
    if (g_efi_os > 1)
    {
        g_efi_os = (grub_strstr(GRUB_PLATFORM, "efi")) ? 1 : 0;
    }

    return g_efi_os;
}

void * ventoy_alloc_chain(grub_size_t size)
{
    void *p = NULL;

    p = grub_malloc(size);
#ifdef GRUB_MACHINE_EFI
    if (!p)
    {
        p = grub_efi_allocate_any_pages(GRUB_EFI_BYTES_TO_PAGES(size));
    }
#endif

    return p;
}

void ventoy_memfile_env_set(const char *prefix, const void *buf, unsigned long long len)
{
    char name[128];
    char val[64];

    grub_snprintf(name, sizeof(name), "%s_addr", prefix);
    grub_snprintf(val, sizeof(val), "0x%llx", (ulonglong)(ulong)buf);
    grub_env_set(name, val);
    
    grub_snprintf(name, sizeof(name), "%s_size", prefix);
    grub_snprintf(val, sizeof(val), "%llu", len);
    grub_env_set(name, val);

    return;
}

static int ventoy_arch_mode_init(void)
{
    #ifdef GRUB_MACHINE_EFI
    if (grub_strcmp(GRUB_TARGET_CPU, "i386") == 0)
    {
        g_ventoy_plat_data = VTOY_PLAT_I386_UEFI;
        grub_snprintf(g_arch_mode_suffix, sizeof(g_arch_mode_suffix), "%s", "ia32");
    }
    else if (grub_strcmp(GRUB_TARGET_CPU, "arm64") == 0)
    {
        g_ventoy_plat_data = VTOY_PLAT_ARM64_UEFI;
        grub_snprintf(g_arch_mode_suffix, sizeof(g_arch_mode_suffix), "%s", "aa64");
    }
    else if (grub_strcmp(GRUB_TARGET_CPU, "mips64el") == 0)
    {
        g_ventoy_plat_data = VTOY_PLAT_MIPS_UEFI;
        grub_snprintf(g_arch_mode_suffix, sizeof(g_arch_mode_suffix), "%s", "mips");
    }
    else
    {
        g_ventoy_plat_data = VTOY_PLAT_X86_64_UEFI;
        grub_snprintf(g_arch_mode_suffix, sizeof(g_arch_mode_suffix), "%s", "uefi");
    }
#else
    g_ventoy_plat_data = VTOY_PLAT_X86_LEGACY;
    grub_snprintf(g_arch_mode_suffix, sizeof(g_arch_mode_suffix), "%s", "legacy");
#endif

    return 0;
}

#ifdef GRUB_MACHINE_EFI
static void ventoy_get_uefi_version(char *str, grub_size_t len)
{
    grub_efi_uint8_t uefi_minor_1, uefi_minor_2;

    uefi_minor_1 = (grub_efi_system_table->hdr.revision & 0xffff) / 10;
    uefi_minor_2 = (grub_efi_system_table->hdr.revision & 0xffff) % 10;
    grub_snprintf(str, len, "%d.%d", (grub_efi_system_table->hdr.revision >> 16), uefi_minor_1);
    if (uefi_minor_2)
        grub_snprintf(str, len, "%s.%d", str, uefi_minor_2);
}
#endif

static int ventoy_calc_totalmem(grub_uint64_t addr, grub_uint64_t size, grub_memory_type_t type, void *data)
{
    grub_uint64_t *total_mem = (grub_uint64_t *)data;

    (void)addr;
    (void)type;

    *total_mem += size;

    return 0;
}

static int ventoy_hwinfo_init(void)
{
    char str[256];
    grub_uint64_t total_mem = 0;

    grub_machine_mmap_iterate(ventoy_calc_totalmem, &total_mem);

    grub_snprintf(str, sizeof(str), "%ld", (long)(total_mem / VTOY_SIZE_1MB));
    ventoy_env_export("grub_total_ram", str);

#ifdef GRUB_MACHINE_EFI
    ventoy_get_uefi_version(str, sizeof(str));
    ventoy_env_export("grub_uefi_version", str);
#else
    ventoy_env_export("grub_uefi_version", "NA");
#endif

    return 0;
}

/* Restore pre-existing GRUB variable callbacks when the module unloads. */
struct ventoy_saved_var_hooks
{
    grub_env_read_hook_t read;
    grub_env_write_hook_t write;
};

static void ventoy_save_var_hooks(const char *name,
                                  struct ventoy_saved_var_hooks *saved)
{
    struct grub_env_var *var;
    saved->read = NULL;
    saved->write = NULL;
    FOR_SORTED_ENV(var)
        if (grub_strcmp(var->name, name) == 0)
        {
            saved->read = var->read_hook;
            saved->write = var->write_hook;
            break;
        }
}

static void ventoy_restore_var_hooks(const char *name,
                                     const struct ventoy_saved_var_hooks *saved,
                                     grub_env_read_hook_t read,
                                     grub_env_write_hook_t write)
{
    struct grub_env_var *var;
    FOR_SORTED_ENV(var)
        if (grub_strcmp(var->name, name) == 0)
        {
            if (var->read_hook == read && var->write_hook == write)
                grub_register_variable_hook(name, saved->read, saved->write);
            break;
        }
}

static global_var_cfg g_global_vars[] = 
{
    { "gfxmode",            "1024x768",   NULL },
    { ventoy_left_key,      "5%",         NULL },
    { ventoy_top_key,       "95%",        NULL },
    { ventoy_color_key,     "#0000ff",    NULL },
    { NULL,                 NULL,         NULL }
};

static struct ventoy_saved_var_hooks
    g_global_saved_hooks[ARRAY_SIZE(g_global_vars) - 1];

static const char * ventoy_global_var_read_hook(struct grub_env_var *var, const char *val)
{
    int i;

    for (i = 0; g_global_vars[i].name; i++)
    {
        if (grub_strcmp(g_global_vars[i].name, var->name) == 0)
        {
            return g_global_vars[i].value;
        }
    }

    return val;
}

static char * ventoy_global_var_write_hook(struct grub_env_var *var, const char *val)
{
    int i;

    for (i = 0; g_global_vars[i].name; i++)
    {
        if (grub_strcmp(g_global_vars[i].name, var->name) == 0)
        {
            grub_check_free(g_global_vars[i].value);
            g_global_vars[i].value = grub_strdup(val);
            break;
        }
    }

    return grub_strdup(val);
}

int ventoy_global_var_init(void)
{
    int i;

    for (i = 0; g_global_vars[i].name; i++)
    {
        ventoy_save_var_hooks(g_global_vars[i].name, &g_global_saved_hooks[i]);
        g_global_vars[i].value = grub_strdup(g_global_vars[i].defval);
        ventoy_env_export(g_global_vars[i].name, g_global_vars[i].defval);        
        grub_register_variable_hook(g_global_vars[i].name, ventoy_global_var_read_hook, ventoy_global_var_write_hook);
    }

    return 0;
}

static void ventoy_global_var_fini(void)
{
    int i;
    for (i = 0; g_global_vars[i].name; i++)
    {
        ventoy_restore_var_hooks(g_global_vars[i].name, &g_global_saved_hooks[i],
                                  ventoy_global_var_read_hook,
                                  ventoy_global_var_write_hook);
        grub_free(g_global_vars[i].value);
        g_global_vars[i].value = NULL;
    }
}

static ctrl_var_cfg g_ctrl_vars[] = 
{
    { "VTOY_WIN11_BYPASS_CHECK",  1 },
    { "VTOY_WIN11_BYPASS_NRO",    1 },
    { "VTOY_LINUX_REMOUNT",       0 },
    { "VTOY_SECONDARY_BOOT_MENU", 1 },
    { NULL, 0 }
};

static struct ventoy_saved_var_hooks
    g_ctrl_saved_hooks[ARRAY_SIZE(g_ctrl_vars) - 1];

static const char * ventoy_ctrl_var_read_hook(struct grub_env_var *var, const char *val)
{
    int i;

    for (i = 0; g_ctrl_vars[i].name; i++)
    {
        if (grub_strcmp(g_ctrl_vars[i].name, var->name) == 0)
        {
            return g_ctrl_vars[i].value ? "1" : "0";
        }
    }

    return val;
}

static char * ventoy_ctrl_var_write_hook(struct grub_env_var *var, const char *val)
{
    int i;

    for (i = 0; g_ctrl_vars[i].name; i++)
    {
        if (grub_strcmp(g_ctrl_vars[i].name, var->name) == 0)
        {
            if (val && val[0] == '1' && val[1] == 0)
            {
                g_ctrl_vars[i].value = 1;
                return grub_strdup("1");
            }
            else
            {
                g_ctrl_vars[i].value = 0;
                return grub_strdup("0");
            }
        }
    }

    return grub_strdup(val);
}

int ventoy_ctrl_var_init(void)
{
    int i;

    for (i = 0; g_ctrl_vars[i].name; i++)
    {
        ventoy_save_var_hooks(g_ctrl_vars[i].name, &g_ctrl_saved_hooks[i]);
        ventoy_env_export(g_ctrl_vars[i].name, g_ctrl_vars[i].value ? "1" : "0");
        grub_register_variable_hook(g_ctrl_vars[i].name, ventoy_ctrl_var_read_hook, ventoy_ctrl_var_write_hook);
    }

    return 0;
}

static void ventoy_ctrl_var_fini(void)
{
    int i;
    for (i = 0; g_ctrl_vars[i].name; i++)
        ventoy_restore_var_hooks(g_ctrl_vars[i].name, &g_ctrl_saved_hooks[i],
                                  ventoy_ctrl_var_read_hook,
                                  ventoy_ctrl_var_write_hook);
}

static grub_command_t ventoy_boot_cmd;

static int
ventoy_fshelp_casefold (void)
{
    return g_ventoy_case_insensitive;
}

static void
ventoy_hashsum_result (const grub_uint8_t *digest, grub_size_t size)
{
    static const char digits[] = "0123456789abcdef";
    char hex[GRUB_CRYPTO_MAX_MDLEN * 2 + 1];
    grub_size_t i;

    if (size > GRUB_CRYPTO_MAX_MDLEN)
        return;
    for (i = 0; i < size; ++i)
    {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[2 * size] = 0;
    grub_env_set("VT_LAST_CHECK_SUM", hex);
}

GRUB_MOD_INIT(ventoy)
{
    ventoy_hwinfo_init();
    ventoy_env_init();
    ventoy_arch_mode_init();
#ifdef GRUB_MACHINE_EFI
    ventoy_secure_boot_init();
#endif
    grub_fshelp_register_casefold_hook(ventoy_fshelp_casefold);
    grub_hashsum_register_result_hook(ventoy_hashsum_result);
    ventoy_linux_opts_init();
    ventoy_register_all_cmd();
    ventoy_boot_cmd = grub_register_command ("ventoy", ventoy_boot_command,
                                           "[--check] [--runtime=DIR] IMAGE.iso",
                                           "Boot a Linux ISO through Ventoy.");
}

GRUB_MOD_FINI(ventoy)
{
    ventoy_linux_opts_fini();
#ifdef GRUB_MACHINE_EFI
    g_ventoy_shim = NULL;
#endif
    g_ventoy_system_sb = 0;
    g_ventoy_sb_policy = 0;
    g_ventoy_sb_policy_applied = 0;
    grub_hashsum_unregister_result_hook(ventoy_hashsum_result);
    grub_fshelp_unregister_casefold_hook(ventoy_fshelp_casefold);
    grub_unregister_command (ventoy_boot_cmd);
    ventoy_unregister_all_cmd();
    ventoy_linux_fini();
    ventoy_env_fini();
    ventoy_compat_fini();
    ventoy_global_var_fini();
    ventoy_ctrl_var_fini();
}
