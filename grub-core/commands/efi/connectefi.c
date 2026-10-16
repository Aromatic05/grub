/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2025  Free Software Foundation, Inc.
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
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/efi/api.h>
#include <grub/efi/pci.h>
#include <grub/efi/efi.h>
#include <grub/efi/disk.h>
#include <grub/command.h>
#include <grub/err.h>
#include <grub/i18n.h>

GRUB_MOD_LICENSE ("GPLv3+");
const int MAX_SEARCH_LOOPS = 100;

struct grub_efi_handle_list
{
  struct grub_efi_handle_list *next;
  struct grub_efi_handle_list **prev;
  grub_efi_handle_t handle;
};
typedef struct grub_efi_handle_list grub_efi_handle_list_t;

enum search_item_flag
{
  SEARCH_NONE = 0,
  SEARCH_LOOP = 1,
  SEARCH_RECURSIVE = 2
};
typedef enum search_item_flag search_item_flag_t;

struct search_item
{
  grub_guid_t guid;
  const char *name;
  search_item_flag_t flags;
};
typedef struct search_item search_item_t;

static bool
is_in_list (grub_efi_handle_t handle, grub_efi_handle_list_t *handles)
{
  grub_efi_handle_list_t *e;

  FOR_LIST_ELEMENTS (e, handles)
    if (e->handle == handle)
      return true;

  return false;
}

static void
free_handle_list (grub_efi_handle_list_t **handles_p)
{
  grub_efi_handle_list_t *e, *n;

  FOR_LIST_ELEMENTS_SAFE (e, n, *handles_p)
    grub_free (e);

  *handles_p = NULL;
}

static grub_err_t
grub_cmd_connectefi (grub_command_t cmd __attribute__ ((unused)),
		     int argc, char **args)
{
  search_item_t pciroot_items[] =
    {
      { GRUB_EFI_PCI_ROOT_IO_GUID, "PCI root", SEARCH_RECURSIVE }
    };
  search_item_t disk_items[] =
    {
      { GRUB_EFI_PCI_ROOT_IO_GUID, "PCI root", SEARCH_NONE },
      { GRUB_EFI_PCI_IO_GUID, "PCI", SEARCH_LOOP },
      { GRUB_EFI_SCSI_IO_PROTOCOL_GUID, "SCSI I/O", SEARCH_RECURSIVE },
      { GRUB_EFI_DISK_IO_GUID, "DISK I/O", SEARCH_RECURSIVE }
    };
  search_item_t *items = NULL;
  int i, nitems = 0;
  grub_err_t err = GRUB_ERR_NONE;
  bool reenumerate_disks = false;
  grub_efi_handle_list_t *already_handled = NULL;
  grub_efi_handle_t *handles = NULL;

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("one argument expected"));

  if (grub_strcmp (args[0], "pciroot") == 0)
    {
      items = pciroot_items;
      nitems = ARRAY_SIZE (pciroot_items);
    }
  else if (grub_strcmp (args[0], "disk") == 0)
    {
      items = disk_items;
      nitems = ARRAY_SIZE (disk_items);
    }
  else if (grub_strcmp (args[0], "all") == 0)
    {
      items = NULL;
      nitems = 1;
    }
  else
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       N_("unexpected argument: `%s'"), args[0]);

  for (i = 0; i < nitems; i++)
    {
      grub_efi_uintn_t num_handles, j;
      int loop = 0;
      bool continue_search;

      do
	{
	  continue_search = false;

	  grub_dprintf ("efi", "item: %s loop: %d\n", (items != NULL) ? items[i].name: "ALL", loop);

	  if (items != NULL)
	    {
	      if (++loop >= MAX_SEARCH_LOOPS)
		{
		  err = grub_error (GRUB_ERR_OUT_OF_RANGE,
			"connectefi: Maximum recursion depth exceeded for item '%s'",
			items[i].name);
		  goto fail;
		}
	      handles = grub_efi_locate_handle (GRUB_EFI_BY_PROTOCOL,
					        &items[i].guid, NULL, &num_handles);
	    }
	  else
	    handles = grub_efi_locate_handle (GRUB_EFI_ALL_HANDLES,
					      NULL, NULL, &num_handles);

	  if (handles == NULL)
	    break; /* Proceed to next item */

	  for (j = 0; j < num_handles; j++)
	    {
	      grub_efi_handle_t handle = handles[j];
	      grub_efi_status_t status;
	      grub_efi_handle_list_t *item;

	      /* Skip already handled handles */
	      if (is_in_list (handle, already_handled))
		{
		  grub_dprintf ("efi", "handle %p: already processed\n", handle);
		  continue;
		}

	      status = grub_efi_system_table->boot_services->connect_controller (
			handle, NULL, NULL,
			(items == NULL) || items[i].flags & SEARCH_RECURSIVE ? 1 : 0);
	      if (status == GRUB_EFI_SUCCESS)
		{
		  if (items != NULL && (items[i].flags & SEARCH_LOOP))
		    continue_search = true;
		  reenumerate_disks = true;
		  grub_dprintf ("efi", "handle %p: connected\n", handle);
		}
	      else
		grub_dprintf ("efi", "handle %p: failed to connect (%"
			      PRIxGRUB_EFI_UINTN_T ")\n", handle, status);

	      item = grub_malloc (sizeof (*item));
	      if (item == NULL)
		{
		  err = grub_errno;
		  goto fail; /* Fatal */
		}
	      grub_list_push (GRUB_AS_LIST_P (&already_handled), GRUB_AS_LIST (item));
	      item->handle = handle;
	    }

	  grub_free (handles);
	  handles = NULL;
	}
      while (continue_search == true);

      /* We are done with this item, proceed to next one */
      free_handle_list (&already_handled);
    }

 fail:
  grub_free (handles);
  free_handle_list (&already_handled);

  if (reenumerate_disks == true)
    grub_efidisk_reenumerate_disks ();

  return err;
}

static grub_command_t cmd;

GRUB_MOD_INIT(connectefi)
{
  cmd = grub_register_command ("connectefi", grub_cmd_connectefi,
			       "pciroot|disk|all",
			       N_("Connect EFI handles."
				  " If 'pciroot' is specified, connect PCI"
				  " root EFI handles recursively."
				  " If 'disk' is specified, connect SCSI"
				  " and DISK I/O EFI handles recursively."
				  " If 'all' is specified, connect all"
				  " EFI handles recursively."));
}

GRUB_MOD_FINI(connectefi)
{
  grub_unregister_command (cmd);
}
