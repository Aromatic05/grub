/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2019  Free Software Foundation, Inc.
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
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/env.h>
#include <grub/err.h>
#include <grub/dl.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>
#include <grub/bind.h>

#include "ini.h"

GRUB_MOD_LICENSE ("GPLv3+");
GRUB_MOD_DUAL_LICENSE ("MIT");

static const struct grub_arg_option options_get[] =
{
  {"set", 's', 0,
   N_("Set a variable to return value."), N_("VARNAME"), ARG_TYPE_STRING},
  {0, 0, 0, 0, 0, 0}
};

enum options_get
{
  INIGET_SET,
};

static grub_err_t
grub_cmd_ini_get (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  if (argc != 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "string required");

  ini_t *config = ini_load (args[0]);
  char *input = NULL;
  char *section = NULL;
  char *key = NULL;
  const char *name = NULL;

  if (!config)
  {
    grub_printf ("cannot parse file: %s\n", args[0]);
    return 0;
  }

  input = grub_strdup (args[1]);
  key = grub_strchr (input, ':');
  if (!key)
    key = input;
  else
  {
    section = input;
    *key = '\0';
    key++;
  }
  name = ini_get(config, section, key);
  if (name)
  {
    if (state[INIGET_SET].set)
      grub_env_set (state[INIGET_SET].arg, name);
    else
      grub_printf("%s%s%s = %s\n", section?:"", section?":":"", key, name);
  }

  if (input)
    grub_free (input);
  ini_free(config);
  return 0;
}

static grub_extcmd_t cmd_get;

/* Native handle ownership is shared by all future script adapters. */
static grub_dl_t ini_module;

static void
destroy_ini (void *data)
{
  ini_free (data);
}

static grub_err_t
bind_ini_load (grub_size_t argc __attribute__ ((unused)),
               const struct grub_bind_value *args,
               struct grub_bind_value *results, grub_size_t *nresults)
{
  ini_t *config = ini_load (args[0].value.bytes.data);
  struct grub_bind_handle *handle;
  results[0].type = GRUB_BIND_NIL;
  *nresults = 1;
  if (!config)
    {
      /* Missing or invalid configuration has historically returned nil. */
      grub_errno = GRUB_ERR_NONE;
      return GRUB_ERR_NONE;
    }
  handle = grub_bind_handle_create (ini_module, "ini", config, destroy_ini);
  if (!handle)
    {
      ini_free (config);
      return grub_errno;
    }
  results[0].type = GRUB_BIND_HANDLE;
  results[0].value.handle = handle;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_ini_free (grub_size_t argc __attribute__ ((unused)),
               const struct grub_bind_value *args,
               struct grub_bind_value *results __attribute__ ((unused)),
               grub_size_t *nresults)
{
  *nresults = 0;
  if (!grub_bind_handle_data (args[0].value.handle, "ini"))
    return grub_errno;
  return grub_bind_handle_close (args[0].value.handle);
}

static grub_err_t
bind_ini_get (grub_size_t argc, const struct grub_bind_value *args,
              struct grub_bind_value *results, grub_size_t *nresults)
{
  ini_t *config = grub_bind_handle_data (args[0].value.handle, "ini");
  const char *section = NULL;
  const char *key, *value;
  if (!config) return grub_errno;
  if (argc == 2)
    key = args[1].value.bytes.data;
  else
    {
      section = args[1].value.bytes.data;
      key = args[2].value.bytes.data;
    }
  value = ini_get (config, section, key);
  results[0].type = GRUB_BIND_NIL;
  *nresults = 1;
  if (value)
    {
      results[0].type = GRUB_BIND_STRING;
      results[0].value.bytes.data = value;
      results[0].value.bytes.size = grub_strlen (value);
    }
  return GRUB_ERR_NONE;
}

static const enum grub_bind_type ini_load_args[] = { GRUB_BIND_STRING };
static const enum grub_bind_type ini_handle_args[] = { GRUB_BIND_HANDLE };
static const enum grub_bind_type ini_get_args[] =
  { GRUB_BIND_HANDLE, GRUB_BIND_STRING, GRUB_BIND_STRING };
static const struct grub_bind_export ini_exports[] =
{
  { "ini", "load", ini_load_args,   1, 1, bind_ini_load },
  { "ini", "free", ini_handle_args, 1, 1, bind_ini_free },
  { "ini", "get",  ini_get_args,    2, 3, bind_ini_get }
};

GRUB_MOD_INIT(ini)
{
  cmd_get = grub_register_extcmd ("ini_get", grub_cmd_ini_get, 0,
                  N_("[--set=VARNAME] FILE [SECTION:]KEY"),
                  N_("Get value from ini files."), options_get);
  ini_module = mod;
  if (grub_bind_register (mod, ini_exports, 3))
    grub_print_error ();
}

GRUB_MOD_FINI(ini)
{
  grub_bind_unregister (ini_module);
  ini_module = NULL;
  grub_unregister_extcmd (cmd_get);
}
