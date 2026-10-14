/* vboot16 - load and start the legacy VBoot BIOS loader. */

#include <grub/command.h>
#include <grub/dl.h>
#include <grub/extcmd.h>
#include <grub/disk.h>
#include <grub/env.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/i386/relocator.h>
#include <grub/loader.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/video.h>

#include "vdl.h"

GRUB_MOD_LICENSE ("GPLv3+");

#define VBOOT_SEGMENT 0x8fc0
#define VBOOT_PAYLOAD (VBOOT_SEGMENT * 16 + 0x200)
#define VBOOT_MAX_SIZE (63 * 1024)

char *vboot_core_addr;

static grub_dl_t module;
static struct grub_relocator *relocator;
static grub_relocator_chunk_t boot_sector_chunk;
static grub_relocator_chunk_t payload_chunk;
static grub_relocator_chunk_t parameter_chunk;

static grub_err_t
vboot16_boot (void)
{
  struct grub_relocator16_state state = {
    .cs = 0, .ip = 0x7c00, .ds = 0, .es = 0, .fs = 0, .gs = 0,
    .ss = 0, .sp = 0x7c00, .edx = 0x80, .a20 = 0
  };

  grub_video_set_mode ("text", 0, 0);
  return grub_relocator16_boot (relocator, state);
}

static grub_err_t
vboot16_unload (void)
{
  if (relocator)
    grub_relocator_unload (relocator);
  relocator = 0;
  grub_dl_unref (module);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_vboot16 (grub_command_t cmd __attribute__ ((unused)), int argc,
                  char **argv)
{
  grub_file_t file;
  grub_uint16_t signature;
  grub_size_t size;
  void *sector, *payload;
  grub_err_t err;

  if (argc < 1 || argc > 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "usage: vboot16 FILE [VHD]");
  if (argc == 2 && grub_strlen (argv[1]) >= 512)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "VHD path must be shorter than 512 bytes");
  file = grub_file_open (argv[0], GRUB_FILE_TYPE_GRUB_MODULE);
  if (!file)
    return grub_errno;
  if (grub_file_size (file) < GRUB_DISK_SECTOR_SIZE
      || grub_file_size (file) > VBOOT_MAX_SIZE)
    {
      grub_file_close (file);
      return grub_error (GRUB_ERR_BAD_OS, "invalid VBoot loader size");
    }

  /* The callbacks use these file-scope chunks; release a prior vboot16
     loader before replacing their state.  */
  grub_loader_unset ();
  relocator = 0;
  relocator = grub_relocator_new ();
  if (!relocator)
    {
      grub_file_close (file);
      return grub_errno;
    }
  err = grub_relocator_alloc_chunk_addr (relocator, &boot_sector_chunk,
                                         0x7c00, GRUB_DISK_SECTOR_SIZE);
  if (err)
    goto fail;
  err = grub_relocator_alloc_chunk_addr (relocator, &payload_chunk,
                                         VBOOT_PAYLOAD,
                                         VBOOT_MAX_SIZE - 0x200);
  if (err)
    goto fail;
  err = grub_relocator_alloc_chunk_addr (relocator, &parameter_chunk,
                                         VBOOT_SEGMENT * 16, 512);
  if (err)
    goto fail;
  sector = get_virtual_current_address (boot_sector_chunk);
  payload = get_virtual_current_address (payload_chunk);
  grub_memset (get_virtual_current_address (parameter_chunk), 0, 512);
  if (argc == 2)
    grub_memcpy (get_virtual_current_address (parameter_chunk), argv[1],
                 grub_strlen (argv[1]));
  if (grub_file_read (file, sector, GRUB_DISK_SECTOR_SIZE)
      != GRUB_DISK_SECTOR_SIZE)
    {
      err = grub_errno ? grub_errno : grub_error (GRUB_ERR_BAD_OS, "short loader file");
      goto fail;
    }
  signature = *(grub_uint16_t *) ((char *) sector + GRUB_DISK_SECTOR_SIZE - 2);
  if (grub_le_to_cpu16 (signature) != 0xaa55)
    {
      err = grub_error (GRUB_ERR_BAD_OS, "invalid signature");
      goto fail;
    }
  size = grub_file_size (file) - GRUB_DISK_SECTOR_SIZE;
  if (size && grub_file_read (file, payload, size) != (grub_ssize_t) size)
    {
      err = grub_errno ? grub_errno : grub_error (GRUB_ERR_FILE_READ_ERROR,
                                                   "could not read loader");
      goto fail;
    }
  *(grub_uint16_t *) ((char *) sector + 6) = grub_cpu_to_le16 (VBOOT_SEGMENT);
  grub_file_close (file);
  grub_dl_ref (module);
  grub_loader_set (vboot16_boot, vboot16_unload, 1);
  return GRUB_ERR_NONE;

 fail:
  grub_file_close (file);
  grub_relocator_unload (relocator);
  relocator = 0;
  return err;
}

static grub_command_t command;
static grub_extcmd_t insmod_command;
static grub_extcmd_t vboot_command;

static void *
vboot_module_span (grub_dl_t mod, grub_uint32_t *size)
{
  grub_dl_segment_t seg;
  grub_addr_t start = ~(grub_addr_t) 0;
  grub_addr_t end = 0;

  for (seg = mod->segment; seg; seg = seg->next)
    if (seg->size)
      {
        grub_addr_t seg_start = (grub_addr_t) seg->addr;
        grub_addr_t seg_end = seg_start + seg->size;

        if (seg_end < seg_start)
          return 0;
        if (seg_start < start)
          start = seg_start;
        if (seg_end > end)
          end = seg_end;
      }

  if (start == ~(grub_addr_t) 0 || end < start || end - start > 0xffffffffU)
    return 0;
  *size = end - start;
  return (void *) start;
}

static grub_err_t
grub_cmd_vboot (grub_extcmd_context_t ctxt __attribute__ ((unused)),
                int argc, char **argv)
{
  const char *root = grub_env_get ("root");
  const char *loader = grub_env_get ("vbootloader");
  const char *core_addr = grub_env_get ("vbootcore_addr");
  char *owned_loader = 0;
  grub_file_t file;
  grub_dl_t mod;
  void *mod_addr;
  grub_uint32_t size;
  char code[32], init[32], addr[32], size_arg[32];
  char **linux_args;
  int i;
  grub_err_t err;

  if (!loader || !*loader)
    {
      grub_size_t len = (root && *root) ? grub_strlen (root) + 15 : 13;
      owned_loader = grub_malloc (len);
      if (!owned_loader)
        return grub_errno;
      if (root && *root)
        grub_snprintf (owned_loader, len, "%s/vboot/vboot", root);
      else
        grub_snprintf (owned_loader, len, "/vboot/vboot");
      loader = owned_loader;
    }

  file = grub_file_open (loader, GRUB_FILE_TYPE_LINUX_KERNEL);
  if (!file)
    {
      err = grub_errno;
      grub_free (owned_loader);
      return err;
    }
  grub_file_close (file);

  vboot_core_addr = 0;
  if (core_addr && *core_addr)
    {
      const char *end;
      grub_uint64_t parsed = grub_strtoull (core_addr, &end, 16);
      if (*end || parsed > 0xffffffffU)
        {
          grub_free (owned_loader);
          return grub_error (GRUB_ERR_BAD_NUMBER, "invalid vbootcore_addr");
        }
      vboot_core_addr = (char *) (grub_addr_t) parsed;
    }

  mod = vboot_dl_load ("vbootcore");
  if (!mod)
    {
      err = grub_errno;
      grub_free (owned_loader);
      return err;
    }
  vboot_dl_ref (mod);
  mod_addr = vboot_module_span (mod, &size);
  if (!mod_addr)
    {
      grub_free (owned_loader);
      return grub_error (GRUB_ERR_BAD_MODULE, "vbootcore has no valid memory span");
    }

  grub_snprintf (code, sizeof (code), "code=%x", (grub_uint32_t) (grub_addr_t) p_text_section_addr);
  grub_snprintf (init, sizeof (init), "init=%x", (grub_uint32_t) (grub_addr_t) mod->init);
  grub_snprintf (addr, sizeof (addr), "addr=%x", (grub_uint32_t) (grub_addr_t) mod_addr);
  grub_snprintf (size_arg, sizeof (size_arg), "size=%x", size);
  linux_args = grub_malloc ((argc + 6) * sizeof (*linux_args));
  if (!linux_args)
    {
      grub_free (owned_loader);
      return grub_errno;
    }
  linux_args[0] = (char *) loader;
  linux_args[1] = code;
  linux_args[2] = init;
  linux_args[3] = addr;
  linux_args[4] = size_arg;
  for (i = 0; i < argc; i++)
    linux_args[i + 5] = argv[i];
  linux_args[argc + 5] = 0;

  err = grub_command_execute ("linux16", argc + 5, linux_args);
  grub_free (linux_args);
  grub_free (owned_loader);
  return err;
}

static grub_err_t
grub_cmd_vbootinsmod (grub_extcmd_context_t ctxt __attribute__ ((unused)),
                      int argc, char **argv)
{
  grub_dl_t loaded;

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "usage: vbootinsmod MODULE");
  if (grub_strchr (argv[0], '/'))
    loaded = vboot_dl_load_file (argv[0]);
  else
    loaded = vboot_dl_load (argv[0]);
  if (!loaded)
    return grub_errno;
  vboot_dl_ref (loaded);
  grub_printf ("module name: %s\ntext section addr: %p\ninit function: %p\n",
               loaded->name, p_text_section_addr, loaded->init);
  return GRUB_ERR_NONE;
}

GRUB_MOD_INIT(vboot)
{
  module = mod;
  command = grub_register_command ("vboot16", grub_cmd_vboot16, "FILE",
                                   "Boot a VBoot-compatible BIOS loader.");
  insmod_command = grub_register_extcmd ("vbootinsmod", grub_cmd_vbootinsmod,
                                         0, "MODULE",
                                         "Load a VBoot ELF module.", 0);
  vboot_command = grub_register_extcmd ("vboot", grub_cmd_vboot, 0,
                                        "[ARGS...]",
                                        "Boot through the VBoot BIOS loader.", 0);
}

GRUB_MOD_FINI(vboot)
{
  grub_unregister_command (command);
  grub_unregister_extcmd (insmod_command);
  grub_unregister_extcmd (vboot_command);
}
