/*
 * Ventoy's Linux boot-option and initrd compatibility layer.
 *
 * Keep legacy vt_* commands in ventoy.mod. The Linux loaders only see
 * the generic argument-vector callback declared by grub/linux.h.
 */
#include <grub/types.h>
#include <grub/err.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/file.h>
#include <grub/command.h>
#include <grub/loader.h>
#include <grub/linux.h>
#include <grub/env.h>
#include <grub/ventoy.h>
#include "ventoy_def.h"

#define VTOY_LINUX_MAX_ARGS 256
#define VTOY_LINUX_MAX_BOOTOPTS 128
#define VTOY_LINUX_MAX_EXTRA 128

static char *vtoy_boot_opts[VTOY_LINUX_MAX_BOOTOPTS];
static unsigned int vtoy_boot_opt_count;
static char *vtoy_extra_initrd[VTOY_LINUX_MAX_EXTRA];
static unsigned int vtoy_extra_count;
static int vtoy_linux_debug;
static int vtoy_initrd_seen;
static int vtoy_linux_kernel_prepared;
static int vtoy_preboot_running;
static const char *vtoy_linux_initrd_command = "initrd";
static struct grub_preboot *vtoy_linux_preboot;
static grub_command_t vtoy_set_cmd, vtoy_unset_cmd;
static grub_command_t vtoy_extra_cmd, vtoy_extra_reset_cmd;

static void
vtoy_linux_clear_options (void)
{
  unsigned int i;
  for (i = 0; i < vtoy_boot_opt_count; ++i)
    grub_free (vtoy_boot_opts[i]);
  grub_memset (vtoy_boot_opts, 0, sizeof (vtoy_boot_opts));
  vtoy_boot_opt_count = 0;
  vtoy_linux_debug = 0;
  vtoy_initrd_seen = 0;
  vtoy_linux_kernel_prepared = 0;
  vtoy_linux_initrd_command = "initrd";
}

static void
vtoy_linux_clear_extra (void)
{
  unsigned int i;
  for (i = 0; i < vtoy_extra_count; ++i)
    grub_free (vtoy_extra_initrd[i]);
  grub_memset (vtoy_extra_initrd, 0, sizeof (vtoy_extra_initrd));
  vtoy_extra_count = 0;
}

static void
vtoy_linux_free_vector (int argc, char **argv)
{
  int i;
  for (i = 0; i < argc; ++i)
    grub_free (argv[i]);
  grub_free (argv);
}

static void
vtoy_linux_trace_args (const char *stage, int argc, char *argv[])
{
  int i;
  for (i = 0; i < argc; ++i)
    grub_dprintf ("ventoyopts", "%s[%d]=%s\n", stage, i, argv[i]);
}

/* Return 1 for an option deliberately removed by original Ventoy.
   Rewriting happens only on owned, duplicated argument strings. */
static int
vtoy_linux_filter_option (char *opt)
{
  if (grub_strcmp (opt, "noinitrd") == 0
      || grub_strcmp (opt, "vga=current") == 0)
    return 1;
  if (grub_strncmp (opt, "rdinit=", 7) == 0)
    {
      if (grub_strcmp (opt, "rdinit=/vtoy/vtoy") != 0)
        {
          opt[0] = 'v';
          opt[1] = 't';
        }
    }
  else if (grub_strncmp (opt, "init=", 5) == 0)
    {
      opt[0] = 'v';
      opt[1] = 't';
    }
  else if (grub_strncmp (opt, "dm=", 3) == 0)
    {
      opt[0] = 'D';
      opt[1] = 'M';
    }
  if (vtoy_linux_debug
      && (grub_strcmp (opt, "quiet") == 0
          || grub_strncmp (opt, "loglevel=", 9) == 0
          || grub_strcmp (opt, "splash") == 0))
    return 1;
  return 0;
}

static grub_err_t
vtoy_linux_append (char **vec, int *count, const char *src, int filter)
{
  char *arg;
  if (*count >= VTOY_LINUX_MAX_ARGS)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "too many Linux arguments");
  arg = grub_strdup (src);
  if (!arg)
    return grub_errno;
  if (filter && vtoy_linux_filter_option (arg))
    {
      grub_free (arg);
      return GRUB_ERR_NONE;
    }
  vec[(*count)++] = arg;
  return GRUB_ERR_NONE;
}

static grub_err_t
vtoy_linux_expand_option (char **vec, int *count, const char *option)
{
  const char *value;
  char *copy, *cur, *end;
  grub_err_t err = GRUB_ERR_NONE;

  if (option[0] != '@')
    return vtoy_linux_append (vec, count, option, 1);
  value = grub_env_get (option + 1);
  if (!value)
    return GRUB_ERR_NONE;
  copy = grub_strdup (value);
  if (!copy)
    return grub_errno;
  for (cur = copy; *cur; cur = end)
    {
      while (*cur == ' ' || *cur == '\t')
        cur++;
      if (!*cur)
        break;
      end = cur;
      while (*end && *end != ' ' && *end != '\t')
        end++;
      if (*end)
        *end++ = 0;
      err = vtoy_linux_append (vec, count, cur, 1);
      if (err)
        break;
    }
  grub_free (copy);
  return err;
}

static grub_err_t
vtoy_linux_compose_initrd (int argc, char *argv[], int *out_count,
                           char ***out_vec, int include_prefix)
{
  const char *addr = grub_env_get ("ventoy_cpio_addr");
  const char *size = grub_env_get ("ventoy_cpio_size");
  const char *part = grub_env_get ("vtoy_img_part_file");
  char cpio[128];
  char **vec;
  int count = 0;
  int i;
  unsigned int j;
  grub_err_t err = GRUB_ERR_NONE;

  vec = grub_calloc (VTOY_LINUX_MAX_ARGS, sizeof (*vec));
  if (!vec)
    return grub_errno;
  if (include_prefix && addr && addr[0] && size && size[0])
    {
      grub_snprintf (cpio, sizeof (cpio), "mem:%s:size:%s", addr, size);
      err = vtoy_linux_append (vec, &count, cpio, 0);
    }
  if (!err && include_prefix && part && part[0])
    err = vtoy_linux_append (vec, &count, part, 0);
  for (j = 0; !err && include_prefix && j < vtoy_extra_count; ++j)
    err = vtoy_linux_append (vec, &count, vtoy_extra_initrd[j], 0);
  for (i = 0; !err && i < argc; ++i)
    err = vtoy_linux_append (vec, &count, argv[i], 0);
  if (err)
    {
      vtoy_linux_free_vector (count, vec);
      return err;
    }
  *out_count = count;
  *out_vec = vec;
  vtoy_linux_trace_args ("initrd", count, vec);
  return GRUB_ERR_NONE;
}

static grub_err_t
vtoy_linux_prepare_args (enum grub_linux_arg_stage stage,
                         int argc, char *argv[], int *out_count,
                         char ***out_vec)
{
  char **vec;
  int count = 0, i;
  unsigned int j;
  grub_err_t err = GRUB_ERR_NONE;

  *out_count = argc;
  *out_vec = argv;
  if (!vtoy_boot_opt_count)
    return GRUB_ERR_NONE;
  if (stage == GRUB_LINUX_ARG_INITRD)
    {
      vtoy_initrd_seen = 1;
      if (vtoy_preboot_running)
        return GRUB_ERR_NONE;
      return vtoy_linux_compose_initrd (argc, argv, out_count, out_vec, 1);
    }
  if (stage != GRUB_LINUX_ARG_KERNEL
      && stage != GRUB_LINUX_ARG_KERNEL16)
    return GRUB_ERR_NONE;

  vec = grub_calloc (VTOY_LINUX_MAX_ARGS, sizeof (*vec));
  if (!vec)
    return grub_errno;

  /* Original argv before '--', then Ventoy opts, then all remaining argv. */
  for (i = 0; i < argc; ++i)
    {
      if (i > 0 && grub_strncmp (argv[i], "--", 2) == 0)
        break;
      err = vtoy_linux_append (vec, &count, argv[i], i > 0);
      if (err)
        goto fail;
    }
  for (j = 0; j < vtoy_boot_opt_count; ++j)
    {
      err = vtoy_linux_expand_option (vec, &count, vtoy_boot_opts[j]);
      if (err)
        goto fail;
    }
  for (; i < argc; ++i)
    {
      err = vtoy_linux_append (vec, &count, argv[i], 1);
      if (err)
        goto fail;
    }
  if (vtoy_linux_debug)
    {
      err = vtoy_linux_append (vec, &count, "loglevel=7", 0);
      if (err)
        goto fail;
    }
  *out_count = count;
  *out_vec = vec;
  vtoy_linux_kernel_prepared = 1;
  vtoy_linux_initrd_command =
    (stage == GRUB_LINUX_ARG_KERNEL16) ? "initrd16" : "initrd";
  vtoy_linux_trace_args ("kernel", count, vec);
  return GRUB_ERR_NONE;
fail:
  vtoy_linux_free_vector (count, vec);
  return err;
}

static grub_err_t
vtoy_linux_set_opt (grub_command_t cmd __attribute__ ((unused)),
                    int argc, char **argv)
{
  int i;
  if ((unsigned int) argc > VTOY_LINUX_MAX_BOOTOPTS - vtoy_boot_opt_count)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "too many Ventoy Linux boot options");
  for (i = 0; i < argc; ++i)
    {
      char *s = grub_strdup (argv[i]);
      if (!s)
        return grub_errno;
      vtoy_boot_opts[vtoy_boot_opt_count++] = s;
    }
  {
    const char *debug = grub_env_get ("vtdebug_flag");
    if (debug && *debug)
      vtoy_linux_debug = 1;
  }
  vtoy_initrd_seen = 0;
  vtoy_linux_kernel_prepared = 0;
  return GRUB_ERR_NONE;
}

static grub_err_t
vtoy_linux_unset_opt (grub_command_t cmd __attribute__ ((unused)),
                      int argc __attribute__ ((unused)),
                      char **argv __attribute__ ((unused)))
{
  vtoy_linux_clear_options ();
  return GRUB_ERR_NONE;
}

static grub_err_t
vtoy_linux_append_extra (grub_command_t cmd __attribute__ ((unused)),
                         int argc, char **argv)
{
  static const char *suffixes[] = { "", ".ko", ".ko.zst", ".ko.xz", ".ko.gz" };
  const char *suffix = NULL;
  const char *name;
  char *path, *entry;
  grub_file_t file;
  unsigned int i;

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "initrd module filename required");
  if (vtoy_extra_count >= VTOY_LINUX_MAX_EXTRA)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "too many Ventoy initrd files");
  for (i = 0; i < ARRAY_SIZE (suffixes); i++)
    {
      path = grub_xasprintf ("%s%s", argv[0], suffixes[i]);
      if (!path)
        return grub_errno;
      file = grub_file_open (path, GRUB_FILE_TYPE_GET_SIZE);
      if (file)
        {
          grub_file_close (file);
          suffix = suffixes[i];
          grub_free (path);
          break;
        }
      grub_free (path);
      grub_errno = GRUB_ERR_NONE;
    }
  if (!suffix)
    return GRUB_ERR_NONE;
  path = grub_xasprintf ("%s%s", argv[0], suffix);
  if (!path)
    return grub_errno;
  name = grub_strrchr (path, '/');
  if (name && grub_strstr (path, ".ko"))
    {
      name++;
      entry = grub_xasprintf ("newc:%s%s:%s", ((110 + grub_strlen (name)) % 4) == 0 ? "." : "",
                             name, path);
      grub_free (path);
      if (!entry)
        return grub_errno;
    }
  else
    entry = path;
  vtoy_extra_initrd[vtoy_extra_count++] = entry;
  return GRUB_ERR_NONE;
}

static grub_err_t
vtoy_linux_reset_extra (grub_command_t cmd __attribute__ ((unused)),
                        int argc __attribute__ ((unused)),
                        char **argv __attribute__ ((unused)))
{
  vtoy_linux_clear_extra ();
  return GRUB_ERR_NONE;
}

/* The original Linux loader supplies Ventoy's initrd even when its GRUB
   menu entry had no explicit 'initrd' command. This generic preboot hook
   preserves that behavior without replacing the 'initrd' command function. */
static grub_err_t
vtoy_linux_preboot_initrd (int noret __attribute__ ((unused)))
{
  const char *addr, *size, *part;
  char cpio[128];
  char *args[VTOY_LINUX_MAX_EXTRA + 2];
  grub_command_t cmd;
  unsigned int i;
  int count = 0;
  grub_err_t err;

  if (!vtoy_boot_opt_count || !vtoy_linux_kernel_prepared || vtoy_initrd_seen
      || !grub_loader_is_loaded ())
    return GRUB_ERR_NONE;
  addr = grub_env_get ("ventoy_cpio_addr");
  size = grub_env_get ("ventoy_cpio_size");
  part = grub_env_get ("vtoy_img_part_file");
  if (addr && addr[0] && size && size[0])
    {
      grub_snprintf (cpio, sizeof (cpio), "mem:%s:size:%s", addr, size);
      args[count++] = cpio;
    }
  if (part && part[0])
    args[count++] = (char *) part;
  for (i = 0; i < vtoy_extra_count; ++i)
    args[count++] = vtoy_extra_initrd[i];
  if (!count)
    return GRUB_ERR_NONE;
  cmd = grub_command_find (vtoy_linux_initrd_command);
  if (!cmd)
    return GRUB_ERR_NONE;
  vtoy_preboot_running = 1;
  vtoy_initrd_seen = 1;
  vtoy_linux_trace_args ("preboot-initrd", count, args);
  err = cmd->func (cmd, count, args);
  vtoy_preboot_running = 0;
  if (err)
    {
      grub_print_error ();
      grub_errno = GRUB_ERR_NONE;
    }
  return GRUB_ERR_NONE;
}

void
ventoy_linux_opts_init (void)
{
  grub_linux_register_args_hook (vtoy_linux_prepare_args,
                                  vtoy_linux_free_vector);
  vtoy_linux_preboot = grub_loader_register_preboot_hook
    (vtoy_linux_preboot_initrd, NULL, GRUB_LOADER_PREBOOT_HOOK_PRIO_NORMAL);
  vtoy_set_cmd = grub_register_command
    ("vt_set_boot_opt", vtoy_linux_set_opt, 0, "Set Ventoy Linux options.");
  vtoy_unset_cmd = grub_register_command
    ("vt_unset_boot_opt", vtoy_linux_unset_opt, 0, "Clear Ventoy Linux options.");
  vtoy_extra_cmd = grub_register_command
    ("vt_img_extra_initrd_append", vtoy_linux_append_extra, 0,
     "Append an extra Ventoy initrd file.");
  vtoy_extra_reset_cmd = grub_register_command
    ("vt_img_extra_initrd_reset", vtoy_linux_reset_extra, 0,
     "Reset extra Ventoy initrd files.");
}

void
ventoy_linux_opts_fini (void)
{
  if (vtoy_linux_preboot)
    grub_loader_unregister_preboot_hook (vtoy_linux_preboot);
  vtoy_linux_preboot = NULL;
  grub_linux_unregister_args_hook (vtoy_linux_prepare_args);
  if (vtoy_set_cmd)
    grub_unregister_command (vtoy_set_cmd);
  if (vtoy_unset_cmd)
    grub_unregister_command (vtoy_unset_cmd);
  if (vtoy_extra_cmd)
    grub_unregister_command (vtoy_extra_cmd);
  if (vtoy_extra_reset_cmd)
    grub_unregister_command (vtoy_extra_reset_cmd);
  vtoy_set_cmd = vtoy_unset_cmd = NULL;
  vtoy_extra_cmd = vtoy_extra_reset_cmd = NULL;
  vtoy_linux_clear_options ();
  vtoy_linux_clear_extra ();
}
