/* geometry.c - report BIOS disk geometry */

#include <grub/dl.h>
#include <grub/env.h>
#include <grub/extcmd.h>
#include <grub/device.h>
#include <grub/disk.h>
#include <grub/i386/pc/biosdisk.h>
#include <grub/i18n.h>
#include <grub/misc.h>

GRUB_MOD_LICENSE ("GPLv3+");

static const struct grub_arg_option options[] =
  {
    {"flag", 'f', 0, N_("Determine flags."), 0, 0},
    {"cylinder", 'c', 0, N_("Determine cylinders."), 0, 0},
    {"head", 'h', 0, N_("Determine heads."), 0, 0},
    {"sector", 's', 0, N_("Determine sectors."), 0, 0},
    {"lba", 'l', 0, N_("Determine if LBA flag is set."), 0, 0},
    {"num", 'n', 0, N_("Determine disk number."), 0, 0},
    {0, 0, 0, 0, 0, 0}
  };

enum { GEOM_FLAG, GEOM_CYLINDER, GEOM_HEAD, GEOM_SECTOR, GEOM_LBA, GEOM_NUM };

static grub_err_t
grub_cmd_geometry (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  grub_disk_t disk;
  struct grub_biosdisk_data *data;
  char ret[64];
  grub_size_t len;

  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "disk name required");

  len = grub_strlen (args[0]);
  if (len >= 2 && args[0][0] == '(' && args[0][len - 1] == ')')
    {
      args[0][len - 1] = '\0';
      disk = grub_disk_open (&args[0][1]);
    }
  else
    disk = grub_disk_open (args[0]);
  if (!disk)
    return grub_error (GRUB_ERR_BAD_DEVICE, "bad disk name");
  if (disk->dev->id != GRUB_DISK_DEVICE_BIOSDISK_ID)
    {
      grub_disk_close (disk);
      return grub_error (GRUB_ERR_BAD_DEVICE, "not a biosdisk");
    }

  data = disk->data;
  if (state[GEOM_FLAG].set)
    grub_snprintf (ret, sizeof (ret), "%lu", data->flags);
  else if (state[GEOM_CYLINDER].set)
    grub_snprintf (ret, sizeof (ret), "%lu", data->cylinders);
  else if (state[GEOM_HEAD].set)
    grub_snprintf (ret, sizeof (ret), "%lu", data->heads);
  else if (state[GEOM_SECTOR].set)
    grub_snprintf (ret, sizeof (ret), "%lu", data->sectors);
  else if (state[GEOM_LBA].set)
    grub_snprintf (ret, sizeof (ret), "%s",
                   (data->flags & GRUB_BIOSDISK_FLAG_LBA) ? "true" : "false");
  else if (state[GEOM_NUM].set)
    grub_snprintf (ret, sizeof (ret), "%d", data->drive);
  else
    {
      char flags[3];
      flags[0] = (data->flags & GRUB_BIOSDISK_FLAG_LBA) ? 'L' : '-';
      flags[1] = (data->flags & GRUB_BIOSDISK_FLAG_CDROM) ? 'C' : '-';
      flags[2] = 0;
      grub_snprintf (ret, sizeof (ret), "%d %s %lu/%lu/%lu", data->drive,
                     flags, data->cylinders, data->heads, data->sectors);
    }
  if (argc > 1)
    grub_env_set (args[1], ret);
  else
    grub_printf ("%s\n", ret);
  grub_disk_close (disk);
  return GRUB_ERR_NONE;
}

static grub_extcmd_t cmd;

GRUB_MOD_INIT(geometry)
{
  cmd = grub_register_extcmd ("geometry", grub_cmd_geometry, 0,
                              N_("OPTION DISK [VAR]"),
                              N_("Retrieve BIOS disk geometry."), options);
}

GRUB_MOD_FINI(geometry)
{
  grub_unregister_extcmd (cmd);
}
