#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/device.h>
#include <grub/term.h>
#include <grub/partition.h>
#include <grub/file.h>
#include <grub/normal.h>
#include <grub/extcmd.h>
#include <grub/datetime.h>
#include <grub/net.h>
#include <grub/time.h>
#include <grub/err.h>
#include <grub/i18n.h>
#include <grub/env.h>
#include <grub/gpt_partition.h>

#include "ventoy_def.h"
#include "ventoy_compat.h"

typedef struct ventoy_chunk_hook_ctx
{
  ventoy_img_chunk_list *chunk_list;
  char *buf;
  grub_uint32_t buf_size;
  grub_uint32_t last_offset;
  grub_uint32_t log_sector_size;
} ventoy_chunk_hook_ctx;

typedef struct grub_vlnk
{
  int srclen;
  char src[512];
  char dst[512];
  struct grub_vlnk *next;
} grub_vlnk;

typedef struct ventoy_file_vlnk
{
  grub_file_t file;
  int vlnk;
  struct ventoy_file_vlnk *next;
} ventoy_file_vlnk;

static int g_ventoy_root_hook_enable = 0;
static int g_ventoy_root_hook_reg = 0;
static grub_env_read_hook_t g_ventoy_root_prev_read;
static grub_env_write_hook_t g_ventoy_root_prev_write;
static grub_env_read_hook_t g_ventoy_menu_lang_read_hook;
static int g_ventoy_iso_nojoliet = 0;
static grub_vlnk g_vtoy_vlnk;
static grub_vlnk *g_vlnk_list;
static ventoy_file_vlnk *g_file_vlnk_list;

/*
 * Compatibility globals: upstream Ventoy keeps these in normal/menu.c and
 * fs/fshelp.c. Keep them in ventoy module scope here to avoid touching core
 * framework files in grub_port.
 */
int g_ventoy_menu_refresh = 0;
int g_ventoy_memdisk_mode = 0;
int g_ventoy_iso_raw = 0;
int g_ventoy_grub2_mode = 0;
int g_ventoy_wimboot_mode = 0;
int g_ventoy_iso_uefi_drv = 0;
int g_ventoy_last_entry = -1;
int g_ventoy_suppress_esc = 0;
int g_ventoy_suppress_esc_default = 1;
int g_ventoy_menu_esc = 0;
int g_ventoy_fn_mutex = 0;
int g_ventoy_secondary_menu_on = 0;
int g_ventoy_case_insensitive = 0;
char g_ventoy_hotkey_tip[256];
char g_ventoy_theme_path[256] = { 0 };

static int g_vtoy_key_num = 0;
static int g_vtoy_key_code[128];

int
ventoy_menu_push_key (int code)
{
  if (g_vtoy_key_num >= 0
      && g_vtoy_key_num < (int) ARRAY_SIZE (g_vtoy_key_code))
    {
      g_vtoy_key_code[g_vtoy_key_num++] = code;
      return 0;
    }

  return -1;
}

/* Helper for ventoy_compat_fs_list_probe.  */
static int
probe_dummy_iter (const char *filename __attribute__ ((unused)),
                  const struct grub_dirhook_info *info __attribute__ ((unused)),
                  void *data __attribute__ ((unused)))
{
  return 1;
}

grub_fs_t
ventoy_compat_fs_list_probe (grub_device_t device, const char **list)
{
  int i;
  grub_fs_t p;

  if (!device->disk)
    return 0;

  for (p = grub_fs_list; p; p = p->next)
    {
      for (i = 0; list[i]; i++)
        {
          if (grub_strcmp (p->name, list[i]) == 0)
            break;
        }

      if (list[i] == NULL)
        continue;

      grub_dprintf ("fs", "Detecting %s...\n", p->name);

      (p->fs_dir) (device, "/", probe_dummy_iter, NULL);
      if (grub_errno == GRUB_ERR_NONE)
        return p;

      grub_error_push ();
      grub_dprintf ("fs", "%s detection failed.\n", p->name);
      grub_error_pop ();

      if (grub_errno != GRUB_ERR_BAD_FS
          && grub_errno != GRUB_ERR_OUT_OF_RANGE)
        return 0;

      grub_errno = GRUB_ERR_NONE;
    }

  return 0;
}

static grub_err_t
ventoy_compat_chunk_append (ventoy_chunk_hook_ctx *ctx, grub_uint64_t sector,
                            grub_uint64_t size)
{
  grub_uint64_t disk_shift;
  grub_uint64_t img_shift;
  ventoy_img_chunk *last_chunk;
  ventoy_img_chunk *new_chunk;

  disk_shift = (size >> ctx->log_sector_size);
  if (disk_shift == 0)
    disk_shift = 1;

  img_shift = (size >> 11);
  if (img_shift == 0)
    img_shift = 1;

  if (ctx->chunk_list->cur_chunk == 0)
    {
      ctx->chunk_list->chunk[0].img_start_sector = 0;
      ctx->chunk_list->chunk[0].img_end_sector = img_shift - 1;
      ctx->chunk_list->chunk[0].disk_start_sector = sector;
      ctx->chunk_list->chunk[0].disk_end_sector = sector + disk_shift - 1;
      ctx->chunk_list->cur_chunk = 1;
      return GRUB_ERR_NONE;
    }

  last_chunk = ctx->chunk_list->chunk + ctx->chunk_list->cur_chunk - 1;
  if (last_chunk->disk_end_sector + 1 == sector)
    {
      last_chunk->img_end_sector += img_shift;
      last_chunk->disk_end_sector += disk_shift;
      return GRUB_ERR_NONE;
    }

  if (ctx->chunk_list->cur_chunk == ctx->chunk_list->max_chunk)
    {
      new_chunk = grub_realloc (ctx->chunk_list->chunk,
                                ctx->chunk_list->max_chunk * 2
                                * sizeof (ventoy_img_chunk));
      if (new_chunk == NULL)
        return GRUB_ERR_OUT_OF_MEMORY;

      ctx->chunk_list->chunk = new_chunk;
      ctx->chunk_list->max_chunk *= 2;
      last_chunk = ctx->chunk_list->chunk + ctx->chunk_list->cur_chunk - 1;
    }

  new_chunk = ctx->chunk_list->chunk + ctx->chunk_list->cur_chunk;
  new_chunk->img_start_sector = last_chunk->img_end_sector + 1;
  new_chunk->img_end_sector = new_chunk->img_start_sector + img_shift - 1;
  new_chunk->disk_start_sector = sector;
  new_chunk->disk_end_sector = sector + disk_shift - 1;
  ctx->chunk_list->cur_chunk++;
  return GRUB_ERR_NONE;
}

static grub_err_t
ventoy_compat_read_hook (grub_disk_addr_t sector, unsigned offset,
                         unsigned length, char *buf, void *data)
{
  ventoy_chunk_hook_ctx *ctx = data;
  char *expect;
  grub_err_t err;

  if (!ctx || !ctx->chunk_list || length == 0)
    return GRUB_ERR_NONE;

  if (offset != 0)
    {
      ctx->chunk_list->err_code = VTOY_CHUNK_ERR_NOT_FLAT;
      return GRUB_ERR_NONE;
    }

  if (ctx->buf != NULL
      && (buf < ctx->buf || buf >= ctx->buf + ctx->buf_size))
    return GRUB_ERR_NONE;

  if (ctx->buf != NULL)
    {
      expect = ctx->buf + ctx->last_offset;
      if (buf != expect)
        {
          ctx->chunk_list->err_code = VTOY_CHUNK_ERR_NOT_FLAT;
          return GRUB_ERR_NONE;
        }

      if (ctx->last_offset + length > ctx->buf_size)
        {
          ctx->chunk_list->err_code = VTOY_CHUNK_ERR_OVER_FLOW;
          return GRUB_ERR_NONE;
        }
    }

  err = ventoy_compat_chunk_append (ctx, sector, length);
  if (err != GRUB_ERR_NONE)
    return err;

  if (ctx->buf != NULL)
    {
      ctx->last_offset += length;
      if (ctx->last_offset == ctx->buf_size)
        ctx->last_offset = 0;
    }

  return GRUB_ERR_NONE;
}

int
ventoy_compat_get_file_chunk (grub_uint64_t part_start, grub_file_t file,
                              ventoy_img_chunk_list *chunk_list)
{
  char *buf;
  grub_off_t size;
  grub_ssize_t read_len;
  grub_off_t cur_read;
  grub_disk_read_hook_t old_read_hook;
  void *old_read_hook_data;
  ventoy_chunk_hook_ctx ctx;

  if (!file || !file->device || !file->device->disk || !chunk_list)
    return 1;

  buf = grub_malloc (VTOY_CHUNK_BUF_SIZE);
  if (!buf)
    return 1;

  grub_memset (&ctx, 0, sizeof (ctx));
  ctx.chunk_list = chunk_list;
  ctx.buf = buf;
  ctx.buf_size = VTOY_CHUNK_BUF_SIZE;
  ctx.log_sector_size = file->device->disk->log_sector_size;

  old_read_hook = file->read_hook;
  old_read_hook_data = file->read_hook_data;

  file->read_hook = ventoy_compat_read_hook;
  file->read_hook_data = &ctx;

  grub_file_seek (file, 0);
  for (size = file->size; size > 0 && chunk_list->err_code == 0; size -= cur_read)
    {
      cur_read = (size > VTOY_CHUNK_BUF_SIZE) ? VTOY_CHUNK_BUF_SIZE : size;
      read_len = grub_file_read (file, buf, cur_read);
      if (read_len <= 0)
        break;
      cur_read = read_len;
    }

  file->read_hook = old_read_hook;
  file->read_hook_data = old_read_hook_data;
  grub_free (buf);

  /*
   * Keep modern-GRUB semantics: disk read hook receives disk-relative sector
   * (partition offset already applied by grub_disk_adjust_range()).
   *
   * Upstream Ventoy 2.04 added part_start here because their patched
   * grub_disk_read() fed partition-relative sectors to blocklist hook.
   * In this port, adding part_start again causes double-offset and invalid LBA
   * (notably on NTFS path).
   */
  (void) part_start;

  return 0;
}

int
ventoy_check_file_exist (const char *fmt, ...)
{
  va_list ap;
  grub_file_t file;
  char fullpath[256];

  grub_memset (fullpath, 0, sizeof (fullpath));
  va_start (ap, fmt);
  grub_vsnprintf (fullpath, sizeof (fullpath) - 1, fmt, ap);
  va_end (ap);

  file = grub_file_open (fullpath, GRUB_FILE_TYPE_NONE);
  if (!file)
    {
      grub_errno = 0;
      return 0;
    }

  grub_file_close (file);
  return 1;
}

int
grub_file_is_vlnk_suffix (const char *name, int len)
{
  grub_uint32_t suffix;

  if (len <= 9)
    return 0;

  suffix = *(const grub_uint32_t *) (name + len - 4);
  if (grub_strncmp (name + len - 9, ".vlnk.", 6) == 0)
    {
      if (suffix == 0x6F73692E || suffix == 0x6D69772E ||
          suffix == 0x676D692E || suffix == 0x6468762E ||
          suffix == 0x6966652E || suffix == 0x7461642E)
        return 1;
    }
  else if (len > 10 && grub_strncmp (name + len - 10, ".vlnk.", 6) == 0)
    {
      if (suffix == 0x78646876 || suffix == 0x796F7476)
        return 1;
    }

  return 0;
}

int
grub_file_vtoy_vlnk (const char *src, const char *dst)
{
  if (src)
    {
      g_vtoy_vlnk.srclen = (int) grub_strlen (src);
      grub_strncpy (g_vtoy_vlnk.src, src, sizeof (g_vtoy_vlnk.src) - 1);
      grub_strncpy (g_vtoy_vlnk.dst, dst, sizeof (g_vtoy_vlnk.dst) - 1);
    }
  else
    {
      g_vtoy_vlnk.srclen = 0;
      g_vtoy_vlnk.src[0] = 0;
      g_vtoy_vlnk.dst[0] = 0;
    }

  return 0;
}

int
grub_file_add_vlnk (const char *src, const char *dst)
{
  grub_vlnk *node;

  if (!src || !dst)
    return 1;

  node = grub_zalloc (sizeof (grub_vlnk));
  if (!node)
    return 1;

  node->srclen = (int) grub_strlen (src);
  grub_strncpy (node->src, src, sizeof (node->src) - 1);
  grub_strncpy (node->dst, dst, sizeof (node->dst) - 1);
  node->next = g_vlnk_list;
  g_vlnk_list = node;
  return 0;
}

const char *
grub_file_get_vlnk (const char *name, int *vlnk)
{
  int len;
  grub_vlnk *node = g_vlnk_list;

  len = grub_strlen (name);
  if (!grub_file_is_vlnk_suffix (name, len))
    return name;

  if (len == g_vtoy_vlnk.srclen && grub_strcmp (name, g_vtoy_vlnk.src) == 0)
    {
      if (vlnk)
        *vlnk = 1;
      return g_vtoy_vlnk.dst;
    }

  while (node)
    {
      if (node->srclen == len && grub_strcmp (name, node->src) == 0)
        {
          if (vlnk)
            *vlnk = 1;
          return node->dst;
        }
      node = node->next;
    }

  return name;
}

void
ventoy_compat_set_file_vlnk (grub_file_t file, int vlnk)
{
  ventoy_file_vlnk *node;

  if (!file)
    return;

  for (node = g_file_vlnk_list; node; node = node->next)
    {
      if (node->file == file)
        {
          node->vlnk = vlnk;
          return;
        }
    }

  node = grub_zalloc (sizeof (ventoy_file_vlnk));
  if (!node)
    return;
  node->file = file;
  node->vlnk = vlnk;
  node->next = g_file_vlnk_list;
  g_file_vlnk_list = node;
}

int
ventoy_compat_get_file_vlnk (grub_file_t file)
{
  ventoy_file_vlnk *node;

  for (node = g_file_vlnk_list; node; node = node->next)
    {
      if (node->file == file)
        return node->vlnk;
    }

  return 0;
}

int
ventoy_compat_get_gpt_priority (grub_disk_t disk, grub_partition_t part,
                                grub_uint32_t *priority)
{
  grub_partition_t p2;
  struct grub_gpt_partentry gptdata;

  if (!disk || !part || !priority)
    return 1;

  if (grub_strcmp (part->partmap->name, "gpt"))
    return 1;

  p2 = disk->partition;
  disk->partition = part->parent;
  if (grub_disk_read (disk, part->offset, part->index, sizeof (gptdata), &gptdata))
    {
      disk->partition = p2;
      return 1;
    }
  disk->partition = p2;

  *priority = (grub_uint32_t) ((grub_le_to_cpu64 (gptdata.attrib) >> 48) & 0xfULL);
  return 0;
}

int
ventoy_compat_efi_secureboot_enabled (void)
{
#ifdef GRUB_MACHINE_EFI
  grub_uint8_t *var = NULL;
  grub_size_t size = 0;
  grub_guid_t global = GRUB_EFI_GLOBAL_VARIABLE_GUID;
  grub_efi_status_t status;
  int enabled = 0;

  status = grub_efi_get_variable ("SecureBoot", &global, &size, (void **) &var);
  if (status == GRUB_EFI_SUCCESS && var && size > 0 && *var == 1)
    enabled = 1;

  grub_free (var);
  return enabled;
#else
  return 0;
#endif
}

static char *
ventoy_compat_root_write (struct grub_env_var *var __attribute__ ((unused)),
                          const char *val)
{
  const char *pos;
  char buf[256];

  if (!g_ventoy_root_hook_enable)
    return grub_strdup (val);

  pos = val;
  if (pos[0] == '(')
    pos++;

  if (grub_strncmp (pos, "vtimghd", 7) == 0)
    return grub_strdup (val);

  pos = grub_strchr (val, ',');
  if (!pos)
    return grub_strdup (val);

  if (val[0] == '(')
    grub_snprintf (buf, sizeof (buf), "(vtimghd%s", pos);
  else
    grub_snprintf (buf, sizeof (buf), "vtimghd%s", pos);

  return grub_strdup (buf);
}

void
ventoy_compat_env_hook_root (int hook)
{
  g_ventoy_root_hook_enable = hook;

  if (hook && !g_ventoy_root_hook_reg)
    {
      struct grub_env_var *var;
      FOR_SORTED_ENV (var)
        if (grub_strcmp (var->name, "root") == 0)
          {
            g_ventoy_root_prev_read = var->read_hook;
            g_ventoy_root_prev_write = var->write_hook;
            break;
          }
      grub_register_variable_hook ("root", 0, ventoy_compat_root_write);
      g_ventoy_root_hook_reg = 1;
    }
}

void
ventoy_compat_fini (void)
{
  grub_vlnk *vlnk;
  ventoy_file_vlnk *file_vlnk;
  struct grub_env_var *var;

  if (g_ventoy_root_hook_reg)
    {
      FOR_SORTED_ENV (var)
        if (grub_strcmp (var->name, "root") == 0)
          {
            if (!var->read_hook
                && var->write_hook == ventoy_compat_root_write)
              grub_register_variable_hook ("root", g_ventoy_root_prev_read,
                                           g_ventoy_root_prev_write);
            break;
          }
    }
  g_ventoy_root_hook_enable = 0;
  g_ventoy_root_hook_reg = 0;
  g_ventoy_root_prev_read = NULL;
  g_ventoy_root_prev_write = NULL;
  g_ventoy_menu_lang_read_hook = NULL;

  while ((vlnk = g_vlnk_list) != NULL)
    {
      g_vlnk_list = vlnk->next;
      grub_free (vlnk);
    }
  while ((file_vlnk = g_file_vlnk_list) != NULL)
    {
      g_file_vlnk_list = file_vlnk->next;
      grub_free (file_vlnk);
    }
  grub_memset (&g_vtoy_vlnk, 0, sizeof (g_vtoy_vlnk));
}

grub_err_t
ventoy_compat_register_menu_lang_hook (grub_env_read_hook_t read_hook)
{
  g_ventoy_menu_lang_read_hook = read_hook;
  return GRUB_ERR_NONE;
}

void *
ventoy_compat_env_get (const char *name)
{
  const char *value;

  if (name && g_ventoy_menu_lang_read_hook
      && grub_strncmp (name, "VTLANG_", 7) == 0)
    return (void *) g_ventoy_menu_lang_read_hook (NULL, name);

  value = grub_env_get (name);
  return (void *) value;
}

void
ventoy_compat_iso9660_set_nojoliet (int nojoliet)
{
  g_ventoy_iso_nojoliet = nojoliet;
}

int
ventoy_compat_iso9660_is_joliet (void)
{
  (void) g_ventoy_iso_nojoliet;
  return 0;
}
