/* One-command Ventoy boot entry point.
 *
 * Keep the legacy vt_* commands as the engine, but do not require the
 * original Ventoy grub.cfg, disk layout or interactive browser to invoke it.
 */
#include <grub/command.h>
#include <grub/device.h>
#include <grub/disk.h>
#include <grub/dl.h>
#include <grub/env.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/partition.h>
#include <grub/types.h>
#include "ventoy_def.h"

#if defined (GRUB_MACHINE_EFI) && defined (__x86_64__)
#define VTOY_BOOT_CONSUMER "ventoy_x64.efi"
#define VTOY_BOOT_FIRSTTRY "FirstTry=@EFI@BOOT@BOOTX64.EFI"
#elif defined (GRUB_MACHINE_EFI) && defined (__aarch64__)
#define VTOY_BOOT_CONSUMER "ventoy_aa64.efi"
#define VTOY_BOOT_FIRSTTRY "FirstTry=@EFI@BOOT@BOOTAA64.EFI"
#elif defined (GRUB_MACHINE_EFI) && defined (__i386__)
#define VTOY_BOOT_CONSUMER "ventoy_ia32.efi"
#define VTOY_BOOT_FIRSTTRY "FirstTry=@EFI@BOOT@BOOTIA32.EFI"
#endif

struct ventoy_boot_search
{
  char *runtime;
};

static int
ventoy_boot_file_exists (const char *path)
{
  grub_file_t file = grub_file_open (path, GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!file)
    {
      grub_errno = GRUB_ERR_NONE;
      return 0;
    }
  grub_file_close (file);
  grub_errno = GRUB_ERR_NONE;
  return 1;
}

static int
ventoy_boot_runtime_valid (const char *dir)
{
#ifdef VTOY_BOOT_CONSUMER
  const char *names[] = { VTOY_COMM_CPIO, VTOY_ARCH_CPIO,
                          VTOY_BOOT_CONSUMER, "iso9660_x64.efi" };
  unsigned i;
  char *path;

  for (i = 0; i < sizeof (names) / sizeof (names[0]); i++)
    {
      /* Only the x64 ISO driver is needed on x64; other architectures
         supply their own driver names through the consumer. */
      if (i == 3 && grub_strcmp (VTOY_BOOT_CONSUMER,
                                "ventoy_x64.efi") != 0)
        continue;
      path = grub_xasprintf ("%s/%s", dir, names[i]);
      if (!path)
        return 0;
      int found = ventoy_boot_file_exists (path);
      grub_free (path);
      if (!found)
        return 0;
    }
  return 1;
#else
  (void) dir;
  return 0;
#endif
}

static char *
ventoy_boot_runtime_from (const char *dir)
{
  if (!dir || !*dir || !ventoy_boot_runtime_valid (dir))
    return NULL;
  return grub_strdup (dir);
}

static int
ventoy_boot_find_runtime (const char *name, void *data)
{
  struct ventoy_boot_search *search = data;
  char *dir = grub_xasprintf ("(%s)/ventoy", name);
  if (!dir)
    return 1;
  search->runtime = ventoy_boot_runtime_from (dir);
  grub_free (dir);
  return search->runtime != NULL || grub_errno == GRUB_ERR_OUT_OF_MEMORY;
}

static char *
ventoy_boot_get_runtime (const char *explicit_dir, const char *image_device)
{
  struct ventoy_boot_search search = { 0 };
  const char *configured = explicit_dir ? explicit_dir : grub_env_get ("ventoy_runtime");
  char *path;

  if (configured)
    {
      search.runtime = ventoy_boot_runtime_from (configured);
      if (!search.runtime && grub_errno == GRUB_ERR_NONE)
        grub_error (GRUB_ERR_FILE_NOT_FOUND,
                    "Ventoy runtime missing or incomplete at %s", configured);
      return search.runtime;
    }

  path = grub_xasprintf ("(%s)/ventoy", image_device);
  if (!path)
    return NULL;
  search.runtime = ventoy_boot_runtime_from (path);
  grub_free (path);
  if (search.runtime)
    return search.runtime;

  /* The image and the EFI consumer need not reside on the same disk.
     For example, the original Ventoy layout uses a dedicated EFI
     partition, but a normal GRUB deployment may use any partition. */
  grub_device_iterate (ventoy_boot_find_runtime, &search);
  if (!search.runtime && grub_errno == GRUB_ERR_NONE)
    grub_error (GRUB_ERR_FILE_NOT_FOUND,
                "Ventoy runtime not found; set ventoy_runtime or use --runtime=DIR");
  return search.runtime;
}

static grub_err_t
ventoy_boot_run (const char *name, int argc, char **args)
{
  grub_err_t err = grub_command_execute (name, argc, args);
  if (err && grub_errno == GRUB_ERR_NONE)
    grub_error (err, "Ventoy stage %s failed", name);
  return err;
}

static grub_err_t
ventoy_boot_ready (const char *name)
{
  if (grub_command_find (name))
    return GRUB_ERR_NONE;
  /* GRUB's user-facing "chainloader" command is supplied by chain.mod. */
  if (!grub_dl_load (grub_strcmp (name, "chainloader") == 0 ? "chain" : name))
    return grub_errno;
  if (!grub_command_find (name))
    return grub_error (GRUB_ERR_FILE_NOT_FOUND, "GRUB command %s unavailable", name);
  return GRUB_ERR_NONE;
}

/* A regular GRUB disk is not necessarily an official Ventoy disk.
   The legacy EFI consumer nevertheless needs its physical MBR/GPT fields
   to identify the *same* backing disk it will read through EFI Block I/O.
   Do not call vt_load_part_table here: that command enforces the original
   Ventoy partition layout and enters an interactive loop on failure. */
static grub_err_t
ventoy_boot_source_disk (const char *device)
{
  char *disk_name = grub_strdup (device);
  char *part;
  grub_disk_t disk = NULL;
  ventoy_gpt_info *table = NULL;
  grub_err_t err = GRUB_ERR_NONE;

  if (!disk_name)
    return grub_errno;
  part = grub_strchr (disk_name, ',');
  if (part)
    *part = 0;
  disk = grub_disk_open (disk_name);
  if (!disk)
    {
      err = grub_errno;
      goto out;
    }
  /* The legacy EFI consumer reads exactly 512 bytes at disk LBA zero.
     Do not silently accept a geometry which that consumer cannot read. */
  if (disk->partition || disk->log_sector_size != GRUB_DISK_SECTOR_BITS ||
      disk->total_sectors == GRUB_DISK_SIZE_UNKNOWN)
    {
      err = grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                        "Ventoy EFI consumer requires a 512-byte physical disk");
      goto out;
    }

  table = grub_zalloc (sizeof (*table));
  if (!table)
    {
      err = grub_errno;
      goto out;
    }
  err = grub_disk_read (disk, 0, 0, sizeof (*table), table);
  if (err)
    goto out;
  if (table->MBR.Byte55 != 0x55 || table->MBR.ByteAA != 0xaa)
    {
      err = grub_error (GRUB_ERR_BAD_PART_TABLE,
                        "Ventoy source disk does not have a valid MBR signature");
      goto out;
    }

  /* GRUB checked the filesystem path and partition before this point.
     This marker tells the old EFI consumer that the direct-boot path
     performed its own checks, not that this is an official Ventoy USB. */
  err = grub_env_set ("VTOY_CHKDEV_RESULT_STRING", "0");
  if (!err)
    err = grub_env_export ("VTOY_CHKDEV_RESULT_STRING");
  if (err)
    goto out;

  grub_free (g_ventoy_part_info);
  g_ventoy_part_info = table;
  table = NULL;
  grub_dprintf ("ventoyboot", "source disk %s loaded (%llu sectors)\n",
                disk_name, (unsigned long long) disk->total_sectors);

out:
  if (disk)
    grub_disk_close (disk);
  grub_free (table);
  grub_free (disk_name);
  return err;
}

static int
ventoy_boot_dir_iter (const char *name __attribute__ ((unused)),
                       const struct grub_dirhook_info *info __attribute__ ((unused)),
                       void *data __attribute__ ((unused)))
{
  return 1;
}

static int
ventoy_boot_directory_exists (const char *path)
{
  grub_device_t dev;
  grub_fs_t fs;
  int exists = 0;
  dev = grub_device_open ("loop");
  if (!dev)
    goto out;
  fs = grub_fs_probe (dev);
  if (fs && fs->fs_dir && fs->fs_dir (dev, path, ventoy_boot_dir_iter, NULL) == GRUB_ERR_NONE)
    exists = 1;
  grub_device_close (dev);
out:
  grub_errno = GRUB_ERR_NONE;
  return exists;
}

/* Extract enough ISO boot metadata for Linux without requiring Ventoy's
   2800-line menu script. Distro-specific overrides remain with the engine. */
static grub_err_t
ventoy_boot_prepare_linux (const char *image, const char *device,
                           const char *relative, const char *runtime)
{
  char *sector_args[] = { (char *) image };
  char *cpio_args[] = { (char *) runtime, (char *) relative,
                        (char *) device, (char *) "busybox=64" };
  char *loop_args[] = { (char *) "loop", (char *) image };
  char *parse_args[] = { (char *) "dir", NULL };
  char *index_args[] = { (char *) "vtoy_boot_initrd_index" };
  char *replace_args[] = { NULL, NULL };
  const char *index;
  grub_err_t err;
  int mounted = 0;

  err = ventoy_boot_run ("vt_img_sector", 1, sector_args);
  if (err)
    return err;
  err = ventoy_boot_run ("vt_load_cpio", 4, cpio_args);
  if (err)
    return err;

  /* ventoy_linux.c currently uses the historical "(loop)" device name.
     Do not steal an already-mounted user's loopback device. */
  {
    grub_disk_t existing = grub_disk_open ("loop");
    if (existing)
      {
        grub_disk_close (existing);
        return grub_error (GRUB_ERR_BAD_ARGUMENT,
                           "loopback device 'loop' is already in use");
      }
    grub_errno = GRUB_ERR_NONE;
  }
  if ((err = ventoy_boot_ready ("loopback")) != GRUB_ERR_NONE)
    return err;
  err = ventoy_boot_run ("loopback", 2, loop_args);
  if (err)
    return err;
  mounted = 1;

  err = ventoy_boot_run ("vt_linux_clear_initrd", 0, NULL);
  if (err)
    goto out;

  if (ventoy_boot_file_exists ("(loop)/sources/boot.wim")
      || ventoy_boot_file_exists ("(loop)/bootmgr")
      || ventoy_boot_file_exists ("(loop)/efi/microsoft/boot/bcd"))
    {
      err = grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                        "Windows ISO requires a separate, validated boot path");
      goto out;
    }

  if (ventoy_boot_directory_exists ("/loader/entries"))
    parse_args[1] = (char *) "(loop)/loader/entries/";
  else if (ventoy_boot_directory_exists ("/boot/grub"))
    parse_args[1] = (char *) "(loop)/boot/grub/";
  if (parse_args[1])
    {
      err = ventoy_boot_run ("vt_linux_parse_initrd_grub", 2, parse_args);
      if (err)
        goto out;
    }

  err = ventoy_boot_run ("vt_linux_locate_initrd", 0, NULL);
  if (err)
    goto out;

  if (parse_args[1] &&
      grub_strcmp (parse_args[1], "(loop)/loader/entries/") == 0)
    {
      err = ventoy_boot_run ("vt_linux_get_main_initrd_index", 1, index_args);
      if (err)
        goto out;
      index = grub_env_get ("vtoy_boot_initrd_index");
      if (index && ventoy_boot_directory_exists ("/arch"))
        {
          const char *replacement = NULL;
          if (ventoy_boot_file_exists ("(loop)/arch/boot/x86_64/archiso.img"))
            replacement = "EFI\\archiso\\archiso.img";
          else if (ventoy_boot_file_exists ("(loop)/arch/boot/x86_64/initramfs-linux.img"))
            replacement = "arch\\boot\\x86_64\\initramfs-linux.img";
          if (replacement)
            {
              replace_args[0] = (char *) index;
              replace_args[1] = (char *) replacement;
              err = ventoy_boot_run ("vt_add_replace_file", 2, replace_args);
              if (err)
                goto out;
            }
        }
    }
out:
  if (mounted)
    {
      char *close_args[] = { (char *) "-d", (char *) "loop" };
      grub_err_t close_err = ventoy_boot_run ("loopback", 2, close_args);
      if (!err)
        err = close_err;
    }
  if (err)
    ventoy_boot_run ("vt_linux_clear_initrd", 0, NULL);
  return err;
}

static grub_err_t
ventoy_boot_consume_linux (const char *image, const char *runtime)
{
#ifdef VTOY_BOOT_CONSUMER
  char *image_args[] = { (char *) image };
  char *acpi_args[2] = { NULL, (char *) "2048" };
  char *loader_args[6] = { NULL, NULL, (char *) "isoefi=on",
                           (char *) VTOY_BOOT_FIRSTTRY,
                           (char *) "iso_fat", NULL };
  const char *address, *size, *param;
  grub_err_t err;

  err = ventoy_boot_run ("vt_linux_chain_data", 1, image_args);
  if (err)
    return err;

  address = grub_env_get ("vtoy_chain_mem_addr");
  size = grub_env_get ("vtoy_chain_mem_size");
  param = grub_env_get ("env_param");
  if (!address || !*address || !size || !*size || !param || !*param)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "Ventoy chain data missing");

  loader_args[0] = grub_xasprintf ("%s/%s", runtime, VTOY_BOOT_CONSUMER);
  loader_args[1] = grub_xasprintf ("env_param=%s", param);
  loader_args[5] = grub_xasprintf ("mem:%s:size:%s", address, size);
  if (!loader_args[0] || !loader_args[1] || !loader_args[5])
    {
      err = grub_errno;
      goto out;
    }

  /* The chain descriptor is handed to an EFI application. Preserve it and
     Ventoy's backing allocations when firmware starts the next loader. */
  acpi_args[0] = (char *) address;
  err = ventoy_boot_ready ("acpi");
  if (err)
    goto out;
  err = ventoy_boot_run ("vt_acpi_param", 2, acpi_args);
  if (err)
    goto out;
  err = ventoy_boot_ready ("chainloader");
  if (err)
    goto out;
  grub_dprintf ("ventoyboot", "chainloader: %s\n", loader_args[0]);
  err = ventoy_boot_run ("chainloader", 6, loader_args);
  if (err)
    goto out;
  grub_dprintf ("ventoyboot", "EFI consumer handoff\n");
  err = ventoy_boot_run ("boot", 0, NULL);
  if (!err)
    err = grub_error (GRUB_ERR_BAD_OS,
                      "Ventoy EFI consumer returned to GRUB");
out:
  grub_free (loader_args[0]);
  grub_free (loader_args[1]);
  grub_free (loader_args[5]);
  return err;
#else
  (void) image;
  (void) runtime;
  return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                     "one-command Ventoy boot is not supported on this architecture");
#endif
}

grub_err_t
ventoy_boot_command (grub_command_t cmd __attribute__ ((unused)),
                     int argc, char **argv)
{
  const char *runtime_arg = NULL;
  const char *image = NULL;
  char *device = NULL, *relative = NULL, *runtime = NULL;
  grub_file_t file = NULL;
  grub_err_t err = GRUB_ERR_NONE;
  const char *ext;
  unsigned i;
  int check_only = 0;

  for (i = 0; i < (unsigned) argc; i++)
    {
      if (grub_strcmp (argv[i], "--check") == 0)
        check_only = 1;
      else if (grub_strncmp (argv[i], "--runtime=", 10) == 0)
        runtime_arg = argv[i] + 10;
      else if (!image && argv[i][0] != '-')
        image = argv[i];
      else
        return grub_error (GRUB_ERR_BAD_ARGUMENT,
                           "usage: ventoy [--check] [--runtime=DIR] (device)/image.iso");
    }
  if (!image || image[0] != '(' || !grub_strchr (image, ')'))
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "ventoy requires an explicit (device)/image.iso path");
  ext = grub_strrchr (image, '.');
  if (!ext || grub_strcasecmp (ext, ".iso") != 0)
    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                       "one-command Ventoy currently supports Linux ISO only");

  device = grub_file_get_device_name (image);
  relative = grub_strchr (image, ')');
  if (!device || !*device || !relative || relative[1] != '/')
    {
      err = grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid GRUB image path");
      goto out;
    }
  relative = grub_strdup (relative + 1);
  if (!relative)
    {
      err = grub_errno;
      goto out;
    }

#ifndef VTOY_BOOT_CONSUMER
  err = grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
                    "one-command Ventoy currently requires EFI x86/ARM");
  goto out;
#endif

  file = grub_file_open (image, GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!file)
    {
      err = grub_errno;
      goto out;
    }
  if (!file->device || !file->device->disk ||
      !file->device->disk->partition || !file->fs ||
      file->size < 0x9000)
    {
      err = grub_error (GRUB_ERR_BAD_ARGUMENT,
                        "Ventoy requires a physical filesystem file on a partition");
      goto out;
    }
  /* Don't misidentify arbitrary files with an .iso suffix. */
  {
    char sig[5];
    grub_file_seek (file, 0x8001);
    if (grub_file_read (file, sig, sizeof (sig)) != sizeof (sig) ||
        grub_memcmp (sig, "CD001", sizeof (sig)) != 0)
      {
        err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid ISO9660 image");
        goto out;
      }
  }

  grub_file_close (file);
  file = NULL;
  runtime = ventoy_boot_get_runtime (runtime_arg, device);
  if (!runtime)
    {
      err = grub_errno;
      goto out;
    }
  if (check_only)
    {
      grub_printf ("Ventoy: ISO readable; runtime %s\n", runtime);
      goto out;
    }

  err = ventoy_boot_source_disk (device);
  if (err)
    goto out;

  grub_env_set ("ventoy_compatible", "NO");
  grub_env_set ("LoadIsoEfiDriver", "on");
  err = ventoy_boot_prepare_linux (image, device, relative, runtime);
  if (!err)
    err = ventoy_boot_consume_linux (image, runtime);
out:
  if (file)
    grub_file_close (file);
  grub_free (device);
  grub_free (relative);
  grub_free (runtime);
  return err;
}
