/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2017  Free Software Foundation, Inc.
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

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/term.h>
#include <grub/env.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>
#include <grub/bind.h>
#include <grub/normal.h>
#include <grub/read.h>

GRUB_MOD_LICENSE ("GPLv3+");

static const struct grub_arg_option options[] =
{
  {0, 'n', 0, N_("grub_getkey_noblock"), 0, 0},
  {0, 0, 0, 0, 0, 0}
};

static grub_err_t
grub_cmd_getkey (grub_extcmd_context_t ctxt, int argc, char **args)

{
  struct grub_arg_list *state = ctxt->state;
  int key;
  char keyenv[20];
  if (state[0].set)
    key = grub_getkey_noblock ();
  else
    key = grub_getkey ();

  if (argc == 1)
  {
    grub_snprintf (keyenv, 20, "%d", key);
    grub_env_set (args[0], keyenv);
  }
  else
  {
    grub_printf ("0x%08x\n", key);
  }
  return GRUB_ERR_NONE;
}

static grub_extcmd_t cmd;

static grub_dl_t input_module;

static grub_err_t
bind_input_read (grub_size_t argc, const struct grub_bind_value *args,
                 struct grub_bind_value *results, grub_size_t *nresults)
{
  int hide = argc ? (int) args[0].value.i64 : 0;
  char *line;
  if (hide < 0 || hide > 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "input.read expects hide 0-2");
  line = grub_getline (hide);
  results[0].type = GRUB_BIND_NIL;
  if (line)
    {
      results[0].type = GRUB_BIND_STRING;
      results[0].owned = 1;
      results[0].value.bytes.data = line;
      results[0].value.bytes.size = grub_strlen (line);
    }
  grub_printf ("\n");
  *nresults = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_input_getkey (grub_size_t argc __attribute__ ((unused)),
                   const struct grub_bind_value *args __attribute__ ((unused)),
                   struct grub_bind_value *results, grub_size_t *nresults)
{
  int c = grub_getkey ();
  results[0].type = GRUB_BIND_I64;
  results[0].value.i64 = c & 0xff;
  results[1].type = GRUB_BIND_I64;
  results[1].value.i64 = (c >> 8) & 0xff;
  *nresults = 2;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_input_getkey_noblock (grub_size_t argc __attribute__ ((unused)),
                           const struct grub_bind_value *args __attribute__ ((unused)),
                           struct grub_bind_value *results, grub_size_t *nresults)
{
  int c = grub_getkey_noblock ();
  results[0].type = GRUB_BIND_I64;
  results[0].value.i64 = c & 0xff;
  results[1].type = GRUB_BIND_I64;
  results[1].value.i64 = (c >> 8) & 0xff;
  *nresults = 2;
  return GRUB_ERR_NONE;
}

static const enum grub_bind_type input_args[] = { GRUB_BIND_I64 };
static const struct grub_bind_export input_exports[] =
{
  { "input", "read",           input_args, 0, 1, bind_input_read },
  { "input", "getkey",         NULL,       0, 0, bind_input_getkey },
  { "input", "getkey_noblock", NULL,       0, 0, bind_input_getkey_noblock }
};

GRUB_MOD_INIT(getkey)
{
  cmd = grub_register_extcmd ("getkey", grub_cmd_getkey, 0,
                              N_("[-n] [VARNAME]"),
                              N_("Return the value of the pressed key. "),
                              options);
  input_module = mod;
  grub_bind_register (mod, input_exports, 3);
}

GRUB_MOD_FINI(getkey)
{
  grub_bind_unregister (input_module);
  input_module = NULL;
  grub_unregister_extcmd (cmd);
}
