/* menuentry.c - menuentry command */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2010  Free Software Foundation, Inc.
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
#include <grub/err.h>
#include <grub/dl.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>
#include <grub/normal.h>
#include <grub/env_private.h>

static const struct grub_arg_option options[] =
  {
    {"class", 1, GRUB_ARG_OPTION_REPEATABLE,
     N_("Menu entry type."), N_("STRING"), ARG_TYPE_STRING},
    {"users", 2, GRUB_ARG_OPTION_OPTIONAL,
     N_("List of users allowed to boot this entry."), N_("USERNAME[,USERNAME]"),
     ARG_TYPE_STRING},
    {"hotkey", 3, 0,
     N_("Keyboard key to quickly boot this entry."), N_("KEYBOARD_KEY"), ARG_TYPE_STRING},
    {"source", 4, 0,
     N_("Use STRING as menu entry body."), N_("STRING"), ARG_TYPE_STRING},
    {"id", 0, 0, N_("Menu entry identifier."), N_("STRING"), ARG_TYPE_STRING},
    /* TRANSLATORS: menu entry can either be bootable by anyone or only by
       handful of users. By default when security is active only superusers can
       boot a given menu entry. With --unrestricted (this option)
       anyone can boot it.  */
    {"unrestricted", 0, 0, N_("This entry can be booted by any user."),
     0, ARG_TYPE_NONE},
    {"help-msg", 0, GRUB_ARG_OPTION_OPTIONAL, N_("Menu entry help message."),
     N_("STRING"), ARG_TYPE_STRING},
    {"submenu", 0, 0, N_("Define a submenu."), 0, ARG_TYPE_NONE},
    {"hidden", 0, 0, N_("Define a hidden menu entry."), 0, ARG_TYPE_NONE},
    {0, 0, 0, 0, 0, 0}
  };

static int
show_hidden_entries_enabled (void)
{
  const char *val;

  val = grub_env_get ("grubfm_show_hidden");
  if (val && val[0] == '1')
    return 1;

  val = grub_env_get ("grub_show_hidden");
  if (val && val[0] == '1')
    return 1;

  return 0;
}

static struct
{
  const char *name;
  int key;
} hotkey_aliases[] =
  {
    {"backspace", GRUB_TERM_BACKSPACE},
    {"tab", GRUB_TERM_TAB},
    {"delete", GRUB_TERM_KEY_DC},
    {"insert", GRUB_TERM_KEY_INSERT},
    {"f1", GRUB_TERM_KEY_F1},
    {"f2", GRUB_TERM_KEY_F2},
    {"f3", GRUB_TERM_KEY_F3},
    {"f4", GRUB_TERM_KEY_F4},
    {"f5", GRUB_TERM_KEY_F5},
    {"f6", GRUB_TERM_KEY_F6},
    {"f7", GRUB_TERM_KEY_F7},
    {"f8", GRUB_TERM_KEY_F8},
    {"f9", GRUB_TERM_KEY_F9},
    {"f10", GRUB_TERM_KEY_F10},
    {"f11", GRUB_TERM_KEY_F11},
    {"f12", GRUB_TERM_KEY_F12},
  };

/* Add a menu entry to the current menu context (as given by the environment
   variable data slot `menu').  As the configuration file is read, the script
   parser calls this when a menu entry is to be created.  */
static grub_err_t
grub_normal_add_menu_entry_internal (int argc, const char **args,
			    char **classes, const char *id,
			    const char *users, const char *hotkey,
			    const char *prefix, const char *sourcecode,
			    const char *help_message,
			    int hidden, int submenu, grub_blsuki_entry_t *blsuki)
{
  int menu_hotkey = 0;
  char **menu_args = NULL;
  char *menu_users = NULL;
  char *menu_title = NULL;
  char *menu_sourcecode = NULL;
  char *menu_id = NULL;
  char *menu_help_message = NULL;
  struct grub_menu_entry_class *menu_classes = NULL;

  grub_menu_t menu;
  grub_menu_entry_t *last;

  menu = grub_env_get_menu ();
  if (! menu)
    {
      grub_dprintf ("normaldbg", "add_menu_entry: no menu context title=%s\n",
                    (argc > 0 && args && args[0]) ? args[0] : "(none)");
    return grub_error (GRUB_ERR_MENU, "no menu context");
    }

  last = &menu->entry_list;

  menu_sourcecode = grub_xasprintf ("%s%s", prefix ?: "", sourcecode);
  if (! menu_sourcecode)
    return grub_errno;

  if (classes && classes[0])
    {
      int i;
      for (i = 0; classes[i]; i++); /* count # of menuentry classes */
      menu_classes = grub_zalloc (sizeof (struct grub_menu_entry_class)
				  * (i + 1));
      if (! menu_classes)
	goto fail;

      for (i = 0; classes[i]; i++)
	{
	  menu_classes[i].name = grub_strdup (classes[i]);
	  if (! menu_classes[i].name)
	    goto fail;
	  menu_classes[i].next = classes[i + 1] ? &menu_classes[i + 1] : NULL;
	}
    }

  if (users)
    {
      menu_users = grub_strdup (users);
      if (! menu_users)
	goto fail;
    }

  if (hotkey)
    {
      unsigned i;
      for (i = 0; i < ARRAY_SIZE (hotkey_aliases); i++)
	if (grub_strcmp (hotkey, hotkey_aliases[i].name) == 0)
	  {
	    menu_hotkey = hotkey_aliases[i].key;
	    break;
	  }
      if (i == ARRAY_SIZE (hotkey_aliases))
	menu_hotkey = hotkey[0];
    }

  if (! argc)
    {
      grub_error (GRUB_ERR_MENU, "menuentry is missing title");
      goto fail;
    }

  menu_title = grub_strdup (args[0]);
  if (! menu_title)
    goto fail;

  menu_id = grub_strdup (id ? : menu_title);
  if (! menu_id)
    goto fail;

  if (help_message)
    {
      menu_help_message = grub_strdup (help_message);
      if (! menu_help_message)
	goto fail;
    }

  /* Save argc, args to pass as parameters to block arg later. */
  menu_args = grub_calloc (argc + 1, sizeof (char *));
  if (! menu_args)
    goto fail;

  {
    int i;
    for (i = 0; i < argc; i++)
      {
	menu_args[i] = grub_strdup (args[i]);
	if (! menu_args[i])
	  goto fail;
      }
    menu_args[argc] = NULL;
  }

  /* Add the menu entry at the end of the list.  */
  while (*last)
    last = &(*last)->next;

  *last = grub_zalloc (sizeof (**last));
  if (! *last)
    goto fail;

  (*last)->title = menu_title;
  (*last)->id = menu_id;
  (*last)->hotkey = menu_hotkey;
  (*last)->hidden = hidden;
  (*last)->classes = menu_classes;
  if (menu_users)
    (*last)->restricted = 1;
  (*last)->users = menu_users;
  (*last)->argc = argc;
  (*last)->args = menu_args;
  (*last)->sourcecode = menu_sourcecode;
  (*last)->help_message = menu_help_message;
  (*last)->submenu = submenu;
  (*last)->blsuki = blsuki;

  if (!hidden)
    menu->size++;
  grub_dprintf ("normaldbg",
		"add_menu_entry: title=%s id=%s size=%d hidden=%d submenu=%d restricted=%d help=%s hotkey_arg=%s hotkey_code=0x%x\n",
                (*last)->title ? (*last)->title : "(null)",
                (*last)->id ? (*last)->id : "(null)",
                menu->size, hidden, submenu, (*last)->restricted,
		(*last)->help_message ? (*last)->help_message : "(none)",
		hotkey ? hotkey : "(none)", menu_hotkey);
  return GRUB_ERR_NONE;

 fail:

  grub_free (menu_sourcecode);
  {
    int i;
    for (i = 0; menu_classes && menu_classes[i].name; i++)
      grub_free (menu_classes[i].name);
    grub_free (menu_classes);
  }

  {
    int i;
    for (i = 0; menu_args && menu_args[i]; i++)
      grub_free (menu_args[i]);
    grub_free (menu_args);
  }

  grub_free (menu_users);
  grub_free (menu_title);
  grub_free (menu_id);
  grub_free (menu_help_message);
  return grub_errno;
}

grub_err_t
grub_normal_add_menu_entry (int argc, const char **args, char **classes,
			    const char *id, const char *users,
			    const char *hotkey, const char *prefix,
			    const char *sourcecode,
			    int hidden, int submenu, grub_blsuki_entry_t *blsuki)
{
  return grub_normal_add_menu_entry_internal (argc, args, classes, id, users,
					     hotkey, prefix, sourcecode, NULL,
					     hidden, submenu, blsuki);
}

static char *
setparams_prefix (int argc, char **args)
{
  int i;
  int j;
  char *p;
  char *result;
  grub_size_t len = 10;

  /* Count resulting string length */
  for (i = 0; i < argc; i++)
    {
      len += 3; /* 3 = 1 space + 2 quotes */
      p = args[i];
      while (*p)
	len += (*p++ == '\'' ? 4 : 1);
    }

  result = grub_malloc (len + 2);
  if (! result)
    return 0;

  grub_strcpy (result, "setparams");
  p = result + 9;

  for (j = 0; j < argc; j++)
    {
      *p++ = ' ';
      *p++ = '\'';
      p = grub_strchrsub (p, args[j], '\'', "'\\''");
      *p++ = '\'';
    }
  *p++ = '\n';
  *p = '\0';
  return result;
}

static grub_err_t
grub_cmd_menuentry (grub_extcmd_context_t ctxt, int argc, char **args)
{
  char ch;
  char *src;
  char *prefix;
  unsigned len;
  grub_err_t r;
  const char *users;
  int hidden, hidden_syntax, submenu;
  char empty_hidden_title[] = "";
  char *empty_hidden_args[1] = { empty_hidden_title };

  submenu = (ctxt->extcmd->cmd->name[0] == 's') || ctxt->state[7].set;
  hidden_syntax = (ctxt->extcmd->cmd->name[0] == 'h') || ctxt->state[8].set;
  hidden = hidden_syntax;
  if (hidden && show_hidden_entries_enabled ())
    hidden = 0;

  if (! argc)
    {
      /* a1ive permits an untitled hidden entry with --source.  Keep an
         empty title without allocating an argv array that can leak.  */
      if (! hidden_syntax || ! ctxt->state[3].set || ctxt->script)
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "missing arguments");
      args = empty_hidden_args;
      argc = 1;
    }

  if (! grub_strlen (args[argc - 1]) && ! hidden_syntax)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "menuentry title is missing");

  if (ctxt->state[3].set && ctxt->script)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "multiple menuentry definitions");

  if (! ctxt->state[3].set && ! ctxt->script)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "no menuentry definition");

  if (ctxt->state[1].set && ctxt->state[1].arg)
    users = ctxt->state[1].arg;
  else if (ctxt->state[5].set)
    users = NULL;
  else
    users = "";

  if (! ctxt->script)
    return grub_normal_add_menu_entry_internal (argc, (const char **) args,
				       (ctxt->state[0].set ? ctxt->state[0].args
					: NULL),
				       ctxt->state[4].arg,
				       users,
				       ctxt->state[2].arg, 0,
				       ctxt->state[3].arg,
				       ctxt->state[6].arg,
				       hidden,
				       submenu,
				       NULL);

  src = args[argc - 1];
  args[argc - 1] = NULL;

  len = grub_strlen(src);
  ch = src[len - 1];
  src[len - 1] = '\0';

  prefix = setparams_prefix (argc - 1, args);
  if (! prefix)
    return grub_errno;

  r = grub_normal_add_menu_entry_internal (argc - 1, (const char **) args,
				  ctxt->state[0].args, ctxt->state[4].arg,
				  users,
				  ctxt->state[2].arg, prefix, src + 1,
				  ctxt->state[6].arg,
				  hidden,
				  submenu, NULL);

  src[len - 1] = ch;
  args[argc - 1] = src;
  grub_free (prefix);
  return r;
}

static grub_err_t
grub_cmd_pop_env (grub_extcmd_context_t ctxt __attribute__ ((unused)),
		  int argc, char **args)
{
  while (argc)
    {
      struct grub_env_context *cc = grub_current_context;
      const char *value = grub_env_get (args[0]);
      if (value)
	{
	  grub_current_context = grub_current_context->prev;
	  while (grub_current_context && grub_env_get (args[0]))
	    {
	      grub_env_set (args[0], value);
	      grub_current_context = grub_current_context->prev;
	    }
	  grub_current_context = cc;
	}
      argc--;
      args++;
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_submenu_exit (grub_extcmd_context_t ctxt __attribute__ ((unused)),
		       int argc __attribute__ ((unused)),
		       char **args __attribute__ ((unused)))
{
  grub_normal_exit_level = -1;
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_clear_menu (grub_command_t cmd __attribute__ ((unused)),
		     int argc __attribute__ ((unused)),
		     char **args __attribute__ ((unused)))
{
  grub_normal_clear_menu ();
  return GRUB_ERR_NONE;
}

void
grub_normal_clear_menu (void)
{
  grub_menu_t menu = grub_env_get_menu ();
  grub_menu_entry_t entry;

  if (!menu)
    {
      grub_dprintf ("normaldbg", "clear_menu: no menu context\n");
    return;
    }
  grub_dprintf ("normaldbg", "clear_menu: before size=%d\n", menu->size);

  entry = menu->entry_list;
  while (entry)
    {
      grub_menu_entry_t next_entry = entry->next;
      grub_size_t i;

      if (entry->classes)
	{
	  struct grub_menu_entry_class *class;
	  for (class = entry->classes; class; class = class->next)
	    grub_free (class->name);
	  grub_free (entry->classes);
	}

      if (entry->args)
	{
	  for (i = 0; entry->args[i]; i++)
	    grub_free (entry->args[i]);
	  grub_free (entry->args);
	}

      if (entry->blsuki)
	entry->blsuki->visible = 0;

      grub_free ((void *) entry->id);
      grub_free ((void *) entry->users);
      grub_free ((void *) entry->title);
      grub_free ((void *) entry->sourcecode);
      grub_free ((void *) entry->help_message);
      grub_free (entry);
      entry = next_entry;
    }

  menu->entry_list = NULL;
  menu->size = 0;
  grub_dprintf ("normaldbg", "clear_menu: after size=%d\n", menu->size);
}

static grub_extcmd_t cmd, cmd_sub, cmd_hidden, cmd_pop, cmd_sub_exit;
static grub_command_t cmd_clear;

void
grub_menu_init (void)
{
  cmd = grub_register_extcmd ("menuentry", grub_cmd_menuentry,
			      GRUB_COMMAND_FLAG_BLOCKS
			      | GRUB_COMMAND_ACCEPT_DASH
			      | GRUB_COMMAND_FLAG_EXTRACTOR,
			      N_("BLOCK"), N_("Define a menu entry."), options);
  cmd_sub = grub_register_extcmd ("submenu", grub_cmd_menuentry,
				  GRUB_COMMAND_FLAG_BLOCKS
				  | GRUB_COMMAND_ACCEPT_DASH
				  | GRUB_COMMAND_FLAG_EXTRACTOR,
				  N_("BLOCK"), N_("Define a submenu."),
				  options);
  cmd_hidden = grub_register_extcmd ("hiddenentry", grub_cmd_menuentry,
				     GRUB_COMMAND_FLAG_BLOCKS
				     | GRUB_COMMAND_ACCEPT_DASH
				     | GRUB_COMMAND_FLAG_EXTRACTOR,
				     N_("BLOCK"), N_("Define a hidden menu entry."),
				     options);
  cmd_pop = grub_register_extcmd ("pop_env", grub_cmd_pop_env, 0,
				  N_("variable_name [...]"),
				  N_("Pass variable value to parent contexts."), 0);
  cmd_sub_exit = grub_register_extcmd ("submenu_exit", grub_cmd_submenu_exit, 0,
				       0, N_("Exit from current submenu."), 0);
  cmd_clear = grub_register_command ("clear_menu", grub_cmd_clear_menu, 0,
				     N_("Clear current menu entries."));
}

void
grub_menu_fini (void)
{
  grub_unregister_extcmd (cmd);
  grub_unregister_extcmd (cmd_sub);
  grub_unregister_extcmd (cmd_hidden);
  grub_unregister_extcmd (cmd_pop);
  grub_unregister_extcmd (cmd_sub_exit);
  grub_unregister_command (cmd_clear);
}
