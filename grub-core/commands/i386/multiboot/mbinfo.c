/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2026 Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/i386/multiboot_info.h>
#include <grub/command.h>
#include <grub/i18n.h>

GRUB_MOD_LICENSE ("GPLv3+");

static grub_err_t
grub_cmd_mbinfo (grub_command_t cmd __attribute__ ((unused)),
		 int argc __attribute__ ((unused)),
		 char *argv[] __attribute__ ((unused)))
{
  const struct multiboot_info *mbi = grub_i386_multiboot_get_info ();

  if (!mbi)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "Multiboot information unavailable");

  grub_printf ("flags: 0x%x\n", (unsigned) mbi->flags);
  if (mbi->flags & MULTIBOOT_INFO_MEMORY)
    grub_printf ("mem_lower: %u\nmem_upper: %u\n",
		 (unsigned) mbi->mem_lower, (unsigned) mbi->mem_upper);
  if (mbi->flags & MULTIBOOT_INFO_CMDLINE)
    grub_printf ("cmdline: %s\n", (char *) (grub_addr_t) mbi->cmdline);
  if (mbi->flags & MULTIBOOT_INFO_BOOT_LOADER_NAME)
    grub_printf ("bootloader: %s\n",
		 (char *) (grub_addr_t) mbi->boot_loader_name);
  if (mbi->flags & MULTIBOOT_INFO_MODS)
    {
      const multiboot_module_t *mod =
	(const void *) (grub_addr_t) mbi->mods_addr;
      grub_uint32_t i;

      grub_printf ("mods_count: %u\nmods_addr: %p\n", mbi->mods_count, mod);
      for (i = 0; i < mbi->mods_count; i++)
	{
	  const char *name = mod[i].cmdline
	    ? (const char *) (grub_addr_t) mod[i].cmdline : "";
	  grub_printf ("[%u] 0x%08x - 0x%08x %s\n", i + 1,
		       mod[i].mod_start, mod[i].mod_end, name);
	}
    }
  if (mbi->flags & MULTIBOOT_INFO_MEM_MAP)
    {
      const grub_uint8_t *cursor = (const void *) (grub_addr_t) mbi->mmap_addr;
      grub_uint32_t remaining = mbi->mmap_length;

      grub_printf ("mmap_addr: %p\nmmap_length: %u\n", cursor, remaining);
      while (remaining >= sizeof (grub_uint32_t))
	{
	  const multiboot_memory_map_t *entry = (const void *) cursor;
	  grub_uint32_t step;

	  if (entry->size < sizeof (*entry) - sizeof (entry->size))
	    break;
	  step = entry->size + sizeof (entry->size);
	  if (step > remaining)
	    break;
	  grub_printf ("size: %u, addr: 0x%llx, length: 0x%llx, type: %u\n",
		       entry->size, (unsigned long long) entry->addr,
		       (unsigned long long) entry->len, entry->type);
	  cursor += step;
	  remaining -= step;
	}
    }
  return GRUB_ERR_NONE;
}

static grub_command_t cmd_mbi;

GRUB_MOD_INIT(mbinfo)
{
  if (grub_i386_multiboot_get_info ())
    cmd_mbi = grub_register_command ("mbinfo", grub_cmd_mbinfo, 0,
				     N_("Display Multiboot info."));
}

GRUB_MOD_FINI(mbinfo)
{
  if (cmd_mbi)
    grub_unregister_command (cmd_mbi);
}
