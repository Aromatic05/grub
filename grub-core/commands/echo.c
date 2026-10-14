/* echo.c - Command to display a line of text  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2006,2007,2010  Free Software Foundation, Inc.
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
#include <grub/extcmd.h>
#include <grub/i18n.h>
#include <grub/term.h>

GRUB_MOD_LICENSE ("GPLv3+");

static const struct grub_arg_option options[] =
  {
    {0, 'n', 0, N_("Do not output the trailing newline."), 0, 0},
    {0, 'e', 0, N_("Enable interpretation of backslash escapes, e.g. "
		   "\\t=tab,\\n=new line,\\e0x1f=white text on blue background."),
     0, 0},
    {0, 0, 0, 0, 0, 0}
  };

static const char *color_list[16] =
  {
    "black", "blue", "green", "cyan", "red", "magenta", "brown",
    "light-gray", "dark-gray", "light-blue", "light-green", "light-cyan",
    "light-red", "light-magenta", "yellow", "white"
  };

static grub_uint8_t
read_xdigit (char num)
{
  if (num >= '0' && num <= '9')
    return num - '0';
  if (num >= 'a' && num <= 'z')
    return num - 'a' + 0x0a;
  if (num >= 'A' && num <= 'Z')
    return num - 'A' + 0x0a;
  return 0;
}

/* Parse an a1ive `\e0xBG` escape (BG = background, FG = foreground).
   Return the number of characters consumed after the backslash, or 0.  */
static int
parse_bash_color (const char *text)
{
  char str[5];
  char src[64];
  grub_uint8_t fg, bg;

  if (! text || grub_strlen (text) < 4)
    return 0;

  grub_memcpy (str, text, 4);
  str[4] = '\0';
  if (str[0] != '0' || (str[1] != 'x' && str[1] != 'X')
      || ! grub_isxdigit (str[2]) || ! grub_isxdigit (str[3]))
    return 0;

  bg = read_xdigit (str[2]);
  fg = read_xdigit (str[3]);
  if (fg > 0x0f || bg > 0x0f)
    return 0;

  grub_snprintf (src, sizeof (src), "set color_normal=%s/%s",
		 color_list[fg], color_list[bg]);
  grub_script_execute_sourcecode (src);
  grub_refresh ();
  return 4;
}

static grub_err_t
grub_cmd_echo (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  int newline = 1;
  int i;

  /* Check if `-n' was used.  */
  if (state[0].set)
    newline = 0;

  for (i = 0; i < argc; i++)
    {
      char *arg = *args;
      /* Unescaping results in a string no longer than the original.  */
      char *unescaped = grub_malloc (grub_strlen (arg) + 1);
      char *p = unescaped;
      args++;

      if (!unescaped)
	return grub_errno;

      while (*arg)
	{
	  /* In case `-e' is used, parse backslashes.  */
	  if (*arg == '\\' && state[1].set)
	    {
	      arg++;
	      if (*arg == '\0')
		break;

	      switch (*arg)
		{
		case '\\':
		  *p++ = '\\';
		  break;

		case 'a':
		  *p++ = '\a';
		  break;

		case 'c':
		  newline = 0;
		  break;

		case 'f':
		  *p++ = '\f';
		  break;

		case 'n':
		  *p++ = '\n';
		  break;

		case 'r':
		  *p++ = '\r';
		  break;

		case 't':
		  *p++ = '\t';
		  break;

		case 'v':
		  *p++ = '\v';
		  break;

		case 'e':
		  if (p != unescaped)
		    {
		      *p = '\0';
		      grub_xputs (unescaped);
		      p = unescaped;
		    }
		  arg += parse_bash_color (arg + 1);
		  break;
		}
	      arg++;
	      continue;
	    }

	  /* This was not an escaped character, or escaping is not
	     enabled.  */
	  *p++ = *arg;
	  arg++;
	}

      *p = '\0';
      grub_xputs (unescaped);
      grub_free (unescaped);

      /* If another argument follows, insert a space.  */
      if (i != argc - 1)
	grub_printf (" " );
    }

  if (newline)
    grub_printf ("\n");

  grub_refresh ();

  return 0;
}

static grub_extcmd_t cmd;

GRUB_MOD_INIT(echo)
{
  cmd = grub_register_extcmd ("echo", grub_cmd_echo,
			      GRUB_COMMAND_ACCEPT_DASH
			      | GRUB_COMMAND_OPTIONS_AT_START,
			      N_("[-e|-n] STRING"), N_("Display a line of text."),
			      options);
}

GRUB_MOD_FINI(echo)
{
  grub_unregister_extcmd (cmd);
}
