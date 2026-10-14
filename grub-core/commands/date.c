/* date.c - command to display/set current datetime.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2008  Free Software Foundation, Inc.
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
#include <grub/err.h>
#include <grub/misc.h>
#include <grub/datetime.h>
#include <grub/command.h>
#include <grub/extcmd.h>
#include <grub/env.h>
#include <grub/mm.h>
#include <grub/i18n.h>

GRUB_MOD_LICENSE ("GPLv3+");

static const struct grub_arg_option options[] =
{
  {"human", 'm', 0, N_("Store date in a human readable format."), 0, 0},
  {"set", 's', 0, N_("Store date in a variable."), N_("VARNAME"),
   ARG_TYPE_STRING},
  {0, 0, 0, 0, 0, 0}
};

enum
{
  DATE_HUMAN,
  DATE_SET
};

#define GRUB_DATETIME_SET_YEAR		1
#define GRUB_DATETIME_SET_MONTH		2
#define GRUB_DATETIME_SET_DAY		4
#define GRUB_DATETIME_SET_HOUR		8
#define GRUB_DATETIME_SET_MINUTE	16
#define GRUB_DATETIME_SET_SECOND	32

static grub_err_t
grub_cmd_date (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  struct grub_datetime datetime;
  int limit[6][2] = {{1980, 2079}, {1, 12}, {1, 31}, {0, 23}, {0, 59}, {0, 59}};
  int value[6], mask;

  if (state[DATE_SET].set)
    {
      char *str;

      if (grub_get_datetime (&datetime))
        return grub_errno;
      if (state[DATE_HUMAN].set)
        str = grub_xasprintf ("%d-%02d-%02d %02d:%02d:%02d %s",
                              datetime.year, datetime.month, datetime.day,
                              datetime.hour, datetime.minute, datetime.second,
                              grub_get_weekday_name (&datetime));
      else
        str = grub_xasprintf ("%d%02d%02d%02d%02d%02d", datetime.year,
                              datetime.month, datetime.day, datetime.hour,
                              datetime.minute, datetime.second);
      if (!str)
        return grub_error (GRUB_ERR_OUT_OF_MEMORY, "out of memory");
      grub_env_set (state[DATE_SET].arg, str);
      grub_free (str);
      return 0;
    }

  if (argc == 0)
    {
      if (grub_get_datetime (&datetime))
        return grub_errno;

      grub_printf ("%d-%02d-%02d %02d:%02d:%02d %s\n",
                   datetime.year, datetime.month, datetime.day,
                   datetime.hour, datetime.minute, datetime.second,
                   grub_get_weekday_name (&datetime));

      return 0;
    }

  grub_memset (&value, 0, sizeof (value));
  mask = 0;

  for (; argc; argc--, args++)
    {
      const char *p;
      char c;
      int m1, ofs, n, cur_mask;

      p = args[0];
      m1 = grub_strtoul (p, &p, 10);

      c = *p;
      if (c == '-')
        ofs = 0;
      else if (c == ':')
        ofs = 3;
      else
        goto fail;

      value[ofs] = m1;
      cur_mask = (1 << ofs);
      mask &= ~(cur_mask * (1 + 2 + 4));

      for (n = 1; (n < 3) && (*p); n++)
        {
          if (*p != c)
            goto fail;

          value[ofs + n] = grub_strtoul (p + 1, &p, 10);
          cur_mask |= (1 << (ofs + n));
        }

      if (*p)
        goto fail;

      if ((ofs == 0) && (n == 2))
        {
          value[ofs + 2] = value[ofs + 1];
          value[ofs + 1] = value[ofs];
          ofs++;
          cur_mask <<= 1;
        }

      for (; n; n--, ofs++)
        if ((value [ofs] < limit[ofs][0]) ||
            (value [ofs] > limit[ofs][1]))
          goto fail;

      mask |= cur_mask;
    }

  if (grub_get_datetime (&datetime))
    return grub_errno;

  if (mask & GRUB_DATETIME_SET_YEAR)
    datetime.year = value[0];

  if (mask & GRUB_DATETIME_SET_MONTH)
    datetime.month = value[1];

  if (mask & GRUB_DATETIME_SET_DAY)
    datetime.day = value[2];

  if (mask & GRUB_DATETIME_SET_HOUR)
    datetime.hour = value[3];

  if (mask & GRUB_DATETIME_SET_MINUTE)
    datetime.minute = value[4];

  if (mask & GRUB_DATETIME_SET_SECOND)
    datetime.second = value[5];

  return grub_set_datetime (&datetime);

fail:
  return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid datetime");
}

static int
date_parse_uint (const char **p, unsigned long *value)
{
  unsigned long n = 0;
  int found = 0;

  while (**p >= '0' && **p <= '9')
    {
      unsigned int digit = *(*p)++ - '0';

      if (n > (~0UL - digit) / 10)
        return 0;
      n = n * 10 + digit;
      found = 1;
    }
  if (found)
    *value = n;
  return found;
}

static void
checktime_help (void)
{
  grub_printf ("Usage: checktime MINUTE HOUR DAY MONTH DAY\n");
  grub_printf ("       * any value; , list separator; - range; / step\n");
}

static grub_err_t
grub_cmd_checktime (grub_command_t cmd __attribute__ ((unused)),
                    int argc, char **args)
{
  struct grub_datetime datetime;
  unsigned int limits[5][2] = {{0, 59}, {0, 23}, {1, 31}, {1, 12}, {0, 7}};
  unsigned int fields[5], i;

  if (argc != 5)
    {
      checktime_help ();
      return GRUB_ERR_NONE;
    }
  if (grub_get_datetime (&datetime))
    return grub_errno;
  fields[0] = datetime.minute;
  fields[1] = datetime.hour;
  fields[2] = datetime.day;
  fields[3] = datetime.month;
  fields[4] = grub_get_weekday (&datetime);

  for (i = 0; i < 5; i++)
    {
      const char *p = args[i];
      int matched = 0;

      do
        {
          unsigned long first, last, step = 1, v;
          int wildcard = *p == '*';

          if (wildcard)
            {
              first = limits[i][0];
              last = limits[i][1];
              p++;
            }
          else
            {
              if (!date_parse_uint (&p, &first))
                goto invalid;
              last = first;
              if (*p == '-')
                {
                  p++;
                  if (!date_parse_uint (&p, &last))
                    goto invalid;
                }
            }
          if (first < limits[i][0] || last > limits[i][1] || first > last)
            goto invalid;
          if (*p == '/')
            {
              p++;
              if (!date_parse_uint (&p, &step) || step == 0)
                goto invalid;
            }
          if (*p && *p != ',')
            goto invalid;
          for (v = first; v <= last; )
            {
              if (v == fields[i])
                matched = 1;
              if (last - v < step)
                break;
              v += step;
            }
          if (!*p)
            break;
          p++;
          if (!*p)
            goto invalid;
        }
      while (1);
      if (!matched)
        return grub_error (GRUB_ERR_TEST_FAILURE, N_("false"));
    }
  return GRUB_ERR_NONE;

invalid:
  return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("invalid time expression"));
}

static grub_extcmd_t cmd_date;
static grub_command_t cmd_checktime;

GRUB_MOD_INIT(date)
{
  cmd_date =
    grub_register_extcmd ("date", grub_cmd_date, 0,
			   N_("[[year-]month-day] [hour:minute[:second]]"),
			   N_("Display/set current datetime."), options);
  cmd_checktime = grub_register_command ("checktime", grub_cmd_checktime,
                                         N_("min hour dom month dow"),
                                         N_("Check current date and time."));
}

GRUB_MOD_FINI(date)
{
  grub_unregister_extcmd (cmd_date);
  grub_unregister_command (cmd_checktime);
}
