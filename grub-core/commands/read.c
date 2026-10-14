/* read.c - Command to read variables from user.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2006,2007,2008  Free Software Foundation, Inc.
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
#include <grub/mm.h>
#include <grub/env.h>
#include <grub/term.h>
#include <grub/types.h>
#include <grub/extcmd.h>
#include <grub/command.h>
#include <grub/file.h>
#include <grub/i18n.h>
#include <grub/safemath.h>
#include <grub/read.h>

GRUB_MOD_LICENSE ("GPLv3+");

static const struct grub_arg_option options[] =
  {
    {"silent", 's', 0, N_("Do not echo input"), 0, 0},
    {0, 0, 0, 0, 0, 0}
  };

char *
grub_getline (int hide_mode)
{
  grub_size_t i;
  char *line;
  char *tmp;
  int c;
  grub_size_t alloc_size;

  i = 0;
  line = grub_malloc (1 + sizeof('\0'));
  if (! line)
    return NULL;

  while (1)
    {
      c = grub_getkey ();
      if ((c == '\n') || (c == '\r'))
	break;

      if (c == GRUB_TERM_BACKSPACE)
	{
	  struct grub_term_output *term;
	  struct grub_term_coordinate pos;

	  if (!i)
	    continue;
	  i--;
	  if (hide_mode == 2)
	    continue;
	  FOR_ACTIVE_TERM_OUTPUTS (term)
	    {
	      if (!term->getxy || !term->gotoxy)
		continue;
	      pos = term->getxy (term);
	      if (pos.x > 0)
		pos.x--;
	      term->gotoxy (term, pos);
	      grub_xputs (" ");
	      term->gotoxy (term, pos);
	    }
	  continue;
	}

      if (!grub_isprint (c))
	continue;

      line[i] = (char) c;
      if (hide_mode == 0)
	grub_printf ("%c", c);
      else if (hide_mode == 1)
	grub_printf ("*");
      if (grub_add (i, 1, &i))
        {
          grub_free (line);
          grub_error (GRUB_ERR_OUT_OF_RANGE, N_("overflow is detected"));
          return NULL;
        }
      if (grub_add (i, 1 + sizeof('\0'), &alloc_size))
        {
          grub_free (line);
          grub_error (GRUB_ERR_OUT_OF_RANGE, N_("overflow is detected"));
          return NULL;
        }
      tmp = grub_realloc (line, alloc_size);
      if (! tmp)
	{
	  grub_free (line);
	  return NULL;
	}
      line = tmp;
    }
  line[i] = '\0';

  return line;
}

static grub_err_t
grub_cmd_read (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  int hide_mode = state[0].set ? 2 : 0;
  const char *mode = NULL;
  char *line;

  if (argc > 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("too many arguments"));

  if (argc == 2)
    {
      mode = args[1];
      if (!grub_strcmp (mode, "asterisk") || !grub_strcmp (mode, "a"))
	hide_mode = 1;
      else if (!grub_strcmp (mode, "hide") || !grub_strcmp (mode, "h"))
	hide_mode = 2;
      else
	return grub_error (GRUB_ERR_BAD_ARGUMENT,
			   N_("mode must be `hide' or `asterisk'"));
    }

  line = grub_getline (hide_mode);

  if (! line)
    return grub_errno;
  if (argc > 0)
    grub_env_set (args[0], line);

  grub_free (line);
  return 0;
}

static grub_err_t
grub_cmd_read_from_file (grub_command_t cmd __attribute__ ((unused)),
			 int argc, char **args)
{
  grub_size_t cap, len, need;
  int rc;
  char ch;
  char *line;
  int i = 0;
  grub_file_t file;

  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("file name expected"));
  if (argc < 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("variable name expected"));

  file = grub_file_open (args[i++], GRUB_FILE_TYPE_CAT);
  if (!file)
    return grub_errno;

  while (i < argc)
    {
      cap = 64;
      len = 0;
      line = grub_malloc (cap);
      if (!line)
	break;

      while (1)
	{
	  rc = grub_file_read (file, &ch, 1);
	  if (rc <= 0)
	    break;
	  if (ch == '\r')
	    continue;
	  if (ch == '\n')
	    break;

	  if (grub_add (len, 2, &need))
	    {
	      grub_free (line);
	      line = NULL;
	      grub_error (GRUB_ERR_OUT_OF_RANGE, N_("overflow is detected"));
	      break;
	    }
	  if (need > cap)
	    {
	      char *tmp = grub_realloc (line, cap * 2);
	      if (!tmp)
		{
		  grub_free (line);
		  line = NULL;
		  break;
		}
	      line = tmp;
	      cap *= 2;
	    }
	  line[len++] = ch;
	}

      if (!line)
	break;
      if (rc < 0)
	{
	  grub_free (line);
	  line = NULL;
	  break;
	}
      if (len == 0 && rc == 0)
	{
	  grub_free (line);
	  line = NULL;
	  break;
	}
      line[len] = '\0';
      grub_env_set (args[i++], line);
      grub_free (line);
    }

  grub_file_close (file);
  if (i != argc)
    return GRUB_ERR_OUT_OF_RANGE;

  return 0;
}

static grub_extcmd_t cmd;
static grub_command_t cme;

GRUB_MOD_INIT(read)
{
  cmd = grub_register_extcmd ("read", grub_cmd_read, 0,
			       N_("[-s] [ENVVAR] [hide|asterisk]"),
			       N_("Set variable with user input."), options);
  cme = grub_register_command ("read_file", grub_cmd_read_from_file,
			       N_("FILE ENVVAR [...]"),
			       N_("Set variable(s) with line(s) from FILE."));
}

GRUB_MOD_FINI(read)
{
  grub_unregister_extcmd (cmd);
  grub_unregister_command (cme);
}
