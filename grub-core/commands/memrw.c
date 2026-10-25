/* memrw.c - command to read / write physical memory  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2009  Free Software Foundation, Inc.
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
#include <grub/env.h>
#include <grub/i18n.h>
#include <grub/file.h>
#include <grub/bind.h>
#ifdef GRUB_MACHINE_EFI
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#else
#include <grub/relocator.h>
#endif

GRUB_MOD_LICENSE ("GPLv3+");

static grub_extcmd_t cmd_read_byte, cmd_read_word, cmd_read_dword;
static grub_command_t cmd_write_byte, cmd_write_word, cmd_write_dword;
static grub_command_t cmd_write_bytes;
static grub_extcmd_t cmd_loadfile;

static const struct grub_arg_option options[] =
  {
    {0, 'v', 0, N_("Save read value into variable VARNAME."),
     N_("VARNAME"), ARG_TYPE_STRING},
    {0, 0, 0, 0, 0, 0}
  };


static grub_err_t
grub_cmd_read (grub_extcmd_context_t ctxt, int argc, char **argv)
{
  grub_addr_t addr;
  grub_uint32_t value = 0;
  char buf[sizeof ("XXXXXXXX")];

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("one argument expected"));

  addr = grub_strtoul (argv[0], 0, 0);
  switch (ctxt->extcmd->cmd->name[sizeof ("read_") - 1])
    {
    case 'd':
      value = *((volatile grub_uint32_t *) addr);
      break;

    case 'w':
      value = *((volatile grub_uint16_t *) addr);
      break;

    case 'b':
      value = *((volatile grub_uint8_t *) addr);
      break;
    }

  if (ctxt->state[0].set)
    {
      grub_snprintf (buf, sizeof (buf), "%x", value);
      grub_env_set (ctxt->state[0].arg, buf);
    }
  else
    grub_printf ("0x%x\n", value);

  return 0;
}

static grub_err_t
grub_cmd_write (grub_command_t cmd, int argc, char **argv)
{
  grub_addr_t addr;
  grub_uint32_t value;
  grub_uint32_t mask = 0xffffffff;

  if (argc != 2 && argc != 3)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("two arguments expected"));

  addr = grub_strtoul (argv[0], 0, 0);
  value = grub_strtoul (argv[1], 0, 0);
  if (argc == 3)
    mask = grub_strtoul (argv[2], 0, 0);
  value &= mask;
  switch (cmd->name[sizeof ("write_") - 1])
    {
    case 'd':
      if (mask != 0xffffffff)
	*((volatile grub_uint32_t *) addr)
	  = (*((volatile grub_uint32_t *) addr) & ~mask) | value;
      else
	*((volatile grub_uint32_t *) addr) = value;
      break;

    case 'w':
      if ((mask & 0xffff) != 0xffff)
	*((volatile grub_uint16_t *) addr)
	  = (*((volatile grub_uint16_t *) addr) & ~mask) | value;
      else
	*((volatile grub_uint16_t *) addr) = value;
      break;

    case 'b':
      if ((mask & 0xff) != 0xff)
	*((volatile grub_uint8_t *) addr)
	  = (*((volatile grub_uint8_t *) addr) & ~mask) | value;
      else
	*((volatile grub_uint8_t *) addr) = value;
      break;
    }

  return 0;
}

static grub_err_t
grub_cmd_write_bytes (grub_command_t cmd __attribute__ ((unused)),
		      int argc, char **argv)
{
  grub_addr_t addr;
  int i;

  if (argc < 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       N_("at least two arguments expected"));

  addr = grub_strtoul (argv[0], 0, 0);
  for (i = 1; i < argc; i++)
    {
      grub_uint32_t value = grub_strtoul (argv[i], 0, 0) & 0xff;
      *((volatile grub_uint8_t *) (addr + (i - 1))) = (grub_uint8_t) value;
    }

  return 0;
}

static const struct grub_arg_option options_loadfile[] =
  {
    {"skip", 'k', 0, N_("Skip N bytes from file."), N_("N"), ARG_TYPE_INT},
    {"length", 'l', 0, N_("Read only N bytes."), N_("N"), ARG_TYPE_INT},
    {"addr", 'a', 0, N_("Specify memory address."), N_("ADDR"), ARG_TYPE_INT},
    {"nodecompress", 'n', 0, N_("Don't decompress the file."), 0, 0},
    {"set", 's', 0, N_("Store loaded memfile path into variable VARNAME."),
     N_("VARNAME"), ARG_TYPE_STRING},
    {0, 0, 0, 0, 0, 0}
  };

enum options_loadfile
  {
    LOADFILE_SKIP,
    LOADFILE_LENGTH,
    LOADFILE_ADDR,
    LOADFILE_NODECOMPRESS,
    LOADFILE_SET
  };

static grub_err_t
grub_cmd_loadfile (grub_extcmd_context_t ctxt, int argc, char **argv)
{
  struct grub_arg_list *state = ctxt->state;
  grub_file_t file = NULL;
  enum grub_file_type type = GRUB_FILE_TYPE_LOOPBACK;
  grub_uint64_t skip = 0, size, len;
  grub_ssize_t nread;
  char mempath[96];
  void *data = NULL;

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("file name required"));

  if (state[LOADFILE_SKIP].set)
    skip = grub_strtoull (state[LOADFILE_SKIP].arg, NULL, 0);
  if (state[LOADFILE_NODECOMPRESS].set)
    type |= GRUB_FILE_TYPE_NO_DECOMPRESS;

  file = grub_file_open (argv[0], type);
  if (!file)
    return grub_errno;

  size = grub_file_size (file);
  if (skip >= size)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, N_("skip is out of file range"));
      goto fail;
    }

  if (state[LOADFILE_LENGTH].set)
    len = grub_strtoull (state[LOADFILE_LENGTH].arg, NULL, 0);
  else
    len = size - skip;

  if (skip + len > size)
    len = size - skip;

  if (state[LOADFILE_ADDR].set)
    {
#ifdef GRUB_MACHINE_EFI
      data = grub_efi_allocate_fixed ((grub_efi_physical_address_t)
				      grub_strtoull (state[LOADFILE_ADDR].arg,
						     0, 0),
				      (grub_efi_uintn_t)
				      ((len + ((1 << 12) - 1)) >> 12));
#else
      struct grub_relocator *rel = NULL;
      grub_relocator_chunk_t ch;

      rel = grub_relocator_new ();
      if (!rel)
	goto fail;
      if (grub_relocator_alloc_chunk_addr (rel, &ch, (grub_phys_addr_t)
					   grub_strtoull (state[LOADFILE_ADDR].arg,
							  0, 0), len))
	{
	  grub_relocator_unload (rel);
	  goto fail;
	}
      data = get_virtual_current_address (ch);
#endif
    }
  else
    data = grub_malloc ((grub_size_t) len);

  if (!data)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));
      goto fail;
    }

  grub_file_seek (file, (grub_off_t) skip);
  nread = grub_file_read (file, data, (grub_size_t) len);
  if (nread < 0 || (grub_uint64_t) nread != len)
    {
      if (!grub_errno)
	grub_error (GRUB_ERR_FILE_READ_ERROR, N_("failed to read full file range"));
      goto fail;
    }

  grub_snprintf (mempath, sizeof (mempath), "(mem)[%p]+[%" PRIuGRUB_UINT64_T "]",
		 data, len);
  if (state[LOADFILE_SET].set)
    grub_env_set (state[LOADFILE_SET].arg, mempath);
  else
    grub_printf ("File: %s\n", mempath);

fail:
  if (file)
    grub_file_close (file);
  return grub_errno;
}

/* Raw physical memory operations remain explicit to this module.
   The generic ABI only describes these existing operations; it cannot
   expose arbitrary C functions, symbols, or create memory mappings. */
static grub_dl_t memrw_module;

#define NATIVE_MEMRW(ctype, name)                                        \
  static grub_err_t                                                    \
  bind_read_##name (grub_size_t argc __attribute__ ((unused)),          \
                    const struct grub_bind_value *args,                \
                    struct grub_bind_value *results, grub_size_t *nout) \
  {                                                                    \
    grub_uint64_t addr = args[0].value.u64;                             \
    if ((grub_uint64_t) (grub_addr_t) addr != addr ||                    \
        (addr & (sizeof (ctype) - 1)))                                    \
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid memory address"); \
    results[0].type = GRUB_BIND_U64;                                    \
    results[0].value.u64 = *(volatile ctype *) (grub_addr_t) addr;       \
    *nout = 1;                                                         \
    return GRUB_ERR_NONE;                                              \
  }                                                                    \
  static grub_err_t                                                    \
  bind_write_##name (grub_size_t argc __attribute__ ((unused)),         \
                     const struct grub_bind_value *args,               \
                     struct grub_bind_value *results __attribute__ ((unused)), \
                     grub_size_t *nout)                                 \
  {                                                                    \
    grub_uint64_t addr = args[0].value.u64;                             \
    if ((grub_uint64_t) (grub_addr_t) addr != addr ||                    \
        (addr & (sizeof (ctype) - 1)))                                    \
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid memory address"); \
    if (args[1].value.u64 > (grub_uint64_t) ((ctype) ~0u))         \
      return grub_error (GRUB_ERR_OUT_OF_RANGE,                        \
                         "memory write value exceeds type width");    \
    *(volatile ctype *) (grub_addr_t) addr = (ctype) args[1].value.u64;  \
    *nout = 0;                                                         \
    return GRUB_ERR_NONE;                                              \
  }

NATIVE_MEMRW (grub_uint8_t, byte)
NATIVE_MEMRW (grub_uint16_t, word)
NATIVE_MEMRW (grub_uint32_t, dword)

static const enum grub_bind_type mem_address_args[] = { GRUB_BIND_U64 };
static const enum grub_bind_type mem_write_args[] =
  { GRUB_BIND_U64, GRUB_BIND_U64 };
static const struct grub_bind_export mem_exports[] =
{
  { "memrw", "read_byte",   mem_address_args, 1, 1, bind_read_byte },
  { "memrw", "read_word",   mem_address_args, 1, 1, bind_read_word },
  { "memrw", "read_dword",  mem_address_args, 1, 1, bind_read_dword },
  { "memrw", "write_byte",  mem_write_args, 2, 2, bind_write_byte },
  { "memrw", "write_word",  mem_write_args, 2, 2, bind_write_word },
  { "memrw", "write_dword", mem_write_args, 2, 2, bind_write_dword }
};

GRUB_MOD_INIT(memrw)
{
  cmd_read_byte =
    grub_register_extcmd ("read_byte", grub_cmd_read, 0,
			  N_("ADDR"),
			  N_("Read 8-bit value from ADDR."),
			  options);
  cmd_read_word =
    grub_register_extcmd ("read_word", grub_cmd_read, 0,
			  N_("ADDR"),
			  N_("Read 16-bit value from ADDR."),
			  options);
  cmd_read_dword =
    grub_register_extcmd ("read_dword", grub_cmd_read, 0,
			  N_("ADDR"),
			  N_("Read 32-bit value from ADDR."),
			  options);
  cmd_write_byte =
    grub_register_command ("write_byte", grub_cmd_write,
			   N_("ADDR VALUE [MASK]"),
			   N_("Write 8-bit VALUE to ADDR."));
  cmd_write_word =
    grub_register_command ("write_word", grub_cmd_write,
			   N_("ADDR VALUE [MASK]"),
			   N_("Write 16-bit VALUE to ADDR."));
  cmd_write_dword =
    grub_register_command ("write_dword", grub_cmd_write,
			   N_("ADDR VALUE [MASK]"),
			   N_("Write 32-bit VALUE to ADDR."));
  cmd_write_bytes =
    grub_register_command ("write_bytes", grub_cmd_write_bytes,
			   N_("ADDR VALUE1 [VALUE2 [VALUE3 ...]]"),
			   N_("Write 8-bit VALUE sequence to ADDR."));
  cmd_loadfile =
    grub_register_extcmd ("loadfile", grub_cmd_loadfile, 0,
			  N_("[OPTIONS] FILE"),
			  N_("Load file content into memory."),
			  options_loadfile);
  memrw_module = mod;
  grub_bind_register (mod, mem_exports, 6);
}

GRUB_MOD_FINI(memrw)
{
  grub_bind_unregister (memrw_module);
  memrw_module = NULL;
  grub_unregister_extcmd (cmd_read_byte);
  grub_unregister_extcmd (cmd_read_word);
  grub_unregister_extcmd (cmd_read_dword);
  grub_unregister_command (cmd_write_byte);
  grub_unregister_command (cmd_write_word);
  grub_unregister_command (cmd_write_dword);
  grub_unregister_command (cmd_write_bytes);
  grub_unregister_extcmd (cmd_loadfile);
}
