/* hexdump.c - command to dump the contents of a file or memory */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2007,2008,2009  Free Software Foundation, Inc.
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
#include <grub/file.h>
#include <grub/disk.h>
#include <grub/misc.h>
#include <grub/lib/hexdump.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>
#include <grub/lockdown.h>
#include <grub/env.h>

GRUB_MOD_LICENSE ("GPLv3+");

static const struct grub_arg_option options[] = {
  {"skip", 's', 0, N_("Skip offset bytes from the beginning of file."), 0,
   ARG_TYPE_INT},
  {"length", 'n', 0, N_("Read only LENGTH bytes."), 0, ARG_TYPE_INT},
  {"quiet", 'q', 0, N_("Don't print error."), 0, 0},
  {"decompress", 'd', 0, N_("Decompress the file."), 0, 0},
  {0, 0, 0, 0, 0, 0}
};

static grub_err_t
grub_cmd_hexdump (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;
  char buf[GRUB_DISK_SECTOR_SIZE * 4];
  grub_ssize_t size, length;
  grub_disk_addr_t skip;
  int namelen;
  enum grub_file_type type = GRUB_FILE_TYPE_HEXCAT;

  if (argc < 1 || argc > 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  namelen = grub_strlen (args[0]);
  skip = (state[0].set) ? grub_strtoull (state[0].arg, 0, 0) : 0;
  length = (state[1].set) ? grub_strtoul (state[1].arg, 0, 0) : 256;
  if (!state[3].set)
    type |= GRUB_FILE_TYPE_NO_DECOMPRESS;

  if (!grub_strcmp (args[0], "(mem)"))
    {
      if (argc == 2)
        return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("variable is only supported for files"));
      if (grub_is_lockdown() == GRUB_LOCKDOWN_ENABLED)
        return grub_error (GRUB_ERR_ACCESS_DENIED, N_("memory reading is disabled in lockdown mode"));
      if (!state[2].set)
        hexdump (skip, (char *) (grub_addr_t) skip, length);
    }
  else if ((args[0][0] == '(') && (args[0][namelen - 1] == ')'))
    {
      grub_disk_t disk;
      grub_disk_addr_t sector;
      grub_size_t ofs;

      args[0][namelen - 1] = 0;
      disk = grub_disk_open (&args[0][1]);
      if (! disk)
        return 0;

      sector = (skip >> (GRUB_DISK_SECTOR_BITS + 2)) * 4;
      ofs = skip & (GRUB_DISK_SECTOR_SIZE * 4 - 1);
      while (length)
        {
          grub_size_t len;

          len = length;
          if (len > sizeof (buf))
            len = sizeof (buf);

          if (grub_disk_read (disk, sector, ofs, len, buf))
            break;

          if (!state[2].set)
            hexdump (skip, buf, len);

          ofs = 0;
          skip += len;
          length -= len;
          sector += 4;
        }

      grub_disk_close (disk);
    }
  else
    {
      grub_file_t file;
      char *contents = 0;
      grub_size_t contents_len = 0;
      grub_size_t contents_capacity = 0;

      file = grub_file_open (args[0], type);
      if (! file)
	return 0;

      file->offset = skip;

      while ((size = grub_file_read (file, buf, sizeof (buf))) > 0)
	{
	  unsigned long len;

	  len = ((length) && (size > length)) ? length : size;
	  if (!state[2].set)
	    hexdump (skip, buf, len);
	  if (argc == 2 && len)
	    {
	      grub_size_t needed = contents_len + len;
	      if (needed < contents_len)
		{
		  grub_file_close (file);
		  grub_free (contents);
		  return grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("file contents too large"));
		}
	      if (needed > contents_capacity)
		{
		  grub_size_t capacity = contents_capacity ? contents_capacity : 256;
		  char *new_contents;
		  while (capacity < needed)
		    {
		      if (capacity > ~(grub_size_t) 0 / 2)
			{
			  capacity = needed;
			  break;
			}
		      capacity *= 2;
		    }
		  new_contents = grub_realloc (contents, capacity);
		  if (!new_contents)
		    {
		      grub_file_close (file);
		      grub_free (contents);
		      return grub_errno;
		    }
		  contents = new_contents;
		  contents_capacity = capacity;
		}
	      grub_memcpy (contents + contents_len, buf, len);
	      contents_len = needed;
	    }
	  skip += len;
	  if (length)
	    {
	      length -= len;
	      if (!length)
		break;
	    }
	}

      grub_file_close (file);

      if (argc == 2)
	{
	  char *escaped;
	  grub_size_t i;
	  if (contents_len > (~(grub_size_t) 0 - 1) / 4)
	    {
	      grub_free (contents);
	      return grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("file contents too large"));
	    }
	  escaped = grub_malloc (contents_len * 4 + 1);
	  if (!escaped)
	    {
	      grub_free (contents);
	      return grub_errno;
	    }
	  for (i = 0; i < contents_len; i++)
	    grub_snprintf (escaped + 4 * i, 5, "\\x%02x", (unsigned char) contents[i]);
	  escaped[contents_len * 4] = 0;
	  grub_env_set (args[1], escaped);
	  grub_free (escaped);
	  grub_free (contents);
	}
    }

  return 0;
}

static grub_extcmd_t cmd;

GRUB_MOD_INIT (hexdump)
{
  cmd = grub_register_extcmd ("hexdump", grub_cmd_hexdump, 0,
		      N_("[OPTIONS] FILE_OR_DEVICE [VARIABLE]"),
			      N_("Show raw contents of a file or memory."),
			      options);
}

GRUB_MOD_FINI (hexdump)
{
  grub_unregister_extcmd (cmd);
}
