/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009  Free Software Foundation, Inc.
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

#include <grub/machine/memory.h>
#include <grub/types.h>
#include <grub/multiboot.h>
#include <grub/err.h>
#include <grub/misc.h>
#include <grub/i386/multiboot_info.h>

/* A pointer to the MBI in its initial location.  */
struct multiboot_info *startup_multiboot_info;

/* The MBI has to be copied to our BSS so that it won't be
   overwritten.  This is its final location.  */
static struct multiboot_info kern_multiboot_info;

#define MBI_STRING_SIZE 256
#define MBI_MODULE_LIMIT 32

static char cmdline[MBI_STRING_SIZE];
static char boot_loader_name[MBI_STRING_SIZE];
static multiboot_module_t modules[MBI_MODULE_LIMIT];
static char module_cmdlines[MBI_MODULE_LIMIT][MBI_STRING_SIZE];
static int mbi_ready;

/* Unfortunately we can't use heap at this point.  But 32 looks like a sane
   limit (used by memtest86).  */
static grub_uint8_t mmap_entries[sizeof (struct multiboot_mmap_entry) * 32];

static int
copy_mbi_string (char *dest, grub_uint32_t source)
{
  grub_size_t i;
  const char *src = (const char *) (grub_addr_t) source;

  if (!source || source > 0xffffffffU - (MBI_STRING_SIZE - 1))
    return 0;
  for (i = 0; i + 1 < MBI_STRING_SIZE; i++)
    {
      dest[i] = src[i];
      if (!src[i])
	return 1;
    }
  dest[MBI_STRING_SIZE - 1] = 0;
  return 1;
}

const struct multiboot_info *
grub_i386_multiboot_get_info (void)
{
  return mbi_ready ? &kern_multiboot_info : 0;
}

void
grub_machine_mmap_init (void)
{
  if (! startup_multiboot_info)
    grub_fatal ("Unable to find Multiboot Information (is CONFIG_MULTIBOOT disabled in coreboot?)");

  /* Move MBI to a safe place.  */
  grub_memmove (&kern_multiboot_info, startup_multiboot_info, sizeof (struct multiboot_info));

  /* These payloads are not copied by this early-boot snapshot.  */
  kern_multiboot_info.flags &= ~(MULTIBOOT_INFO_AOUT_SYMS | MULTIBOOT_INFO_ELF_SHDR
				 | MULTIBOOT_INFO_DRIVE_INFO | MULTIBOOT_INFO_CONFIG_TABLE
				 | MULTIBOOT_INFO_APM_TABLE | MULTIBOOT_INFO_VBE_INFO
				 | MULTIBOOT_INFO_FRAMEBUFFER_INFO);

  if (kern_multiboot_info.flags & MULTIBOOT_INFO_CMDLINE)
    {
      if (copy_mbi_string (cmdline, kern_multiboot_info.cmdline))
	kern_multiboot_info.cmdline = (grub_uint32_t) (grub_addr_t) cmdline;
      else
	kern_multiboot_info.flags &= ~MULTIBOOT_INFO_CMDLINE;
    }
  if (kern_multiboot_info.flags & MULTIBOOT_INFO_BOOT_LOADER_NAME)
    {
      if (copy_mbi_string (boot_loader_name, kern_multiboot_info.boot_loader_name))
	kern_multiboot_info.boot_loader_name = (grub_uint32_t) (grub_addr_t) boot_loader_name;
      else
	kern_multiboot_info.flags &= ~MULTIBOOT_INFO_BOOT_LOADER_NAME;
    }
  if (kern_multiboot_info.flags & MULTIBOOT_INFO_MODS)
    {
      grub_uint32_t count = kern_multiboot_info.mods_count;
      multiboot_module_t *source = (void *) (grub_addr_t) kern_multiboot_info.mods_addr;
      grub_uint32_t i;

      if (!source)
	count = 0;
      else if (count > MBI_MODULE_LIMIT)
	count = MBI_MODULE_LIMIT;
      if (count && (kern_multiboot_info.mods_addr > 0xffffffffU
		    - count * sizeof (*source)))
	count = 0;
      for (i = 0; i < count; i++)
	{
	  grub_memmove (&modules[i], &source[i], sizeof (modules[i]));
	  if (copy_mbi_string (module_cmdlines[i], modules[i].cmdline))
	    modules[i].cmdline = (grub_uint32_t) (grub_addr_t) module_cmdlines[i];
	  else
	    modules[i].cmdline = 0;
	}
      kern_multiboot_info.mods_count = count;
      kern_multiboot_info.mods_addr = (grub_uint32_t) (grub_addr_t) modules;
      if (!count)
	kern_multiboot_info.flags &= ~MULTIBOOT_INFO_MODS;
    }

  if ((kern_multiboot_info.flags & MULTIBOOT_INFO_MEM_MAP) == 0)
    grub_fatal ("Missing Multiboot memory information");

  /* Move the memory map to a safe place.  */
  if (!kern_multiboot_info.mmap_addr || !kern_multiboot_info.mmap_length
      || kern_multiboot_info.mmap_addr > 0xffffffffU - kern_multiboot_info.mmap_length)
    grub_fatal ("Invalid Multiboot memory information");
  if (kern_multiboot_info.mmap_length > sizeof (mmap_entries))
    {
      grub_printf ("WARNING: Memory map size exceeds limit (0x%x > 0x%x); it will be truncated\n",
		   kern_multiboot_info.mmap_length, sizeof (mmap_entries));
      kern_multiboot_info.mmap_length = sizeof (mmap_entries);
    }
  /* Keep only complete, structurally valid records in the bounded copy.  */
  {
    grub_uint32_t copied = 0;
    grub_uint32_t limit = kern_multiboot_info.mmap_length;
    const grub_uint8_t *source = (const void *) (grub_addr_t) kern_multiboot_info.mmap_addr;

    while (limit - copied >= sizeof (grub_uint32_t))
      {
	grub_uint32_t size;
	grub_uint32_t step;
	grub_memmove (&size, source + copied, sizeof (size));
	if (size < sizeof (struct multiboot_mmap_entry) - sizeof (size)
	    || size > 0xffffffffU - sizeof (size))
	  break;
	step = size + sizeof (size);
	if (step > limit - copied)
	  break;
	copied += step;
      }
    kern_multiboot_info.mmap_length = copied;
    if (!copied)
      grub_fatal ("Invalid Multiboot memory map");
  }
  if (kern_multiboot_info.mmap_length)
    {
      grub_memmove (mmap_entries, (void *) kern_multiboot_info.mmap_addr, kern_multiboot_info.mmap_length);
      kern_multiboot_info.mmap_addr = (grub_uint32_t) (grub_addr_t) mmap_entries;
    }
  mbi_ready = 1;
}

grub_err_t
grub_machine_mmap_iterate (grub_memory_hook_t hook, void *hook_data)
{
  struct multiboot_mmap_entry *entry = (void *) kern_multiboot_info.mmap_addr;

  grub_uint32_t remaining = kern_multiboot_info.mmap_length;

  while (remaining >= sizeof (entry->size))
    {
      grub_uint32_t step;

      if (entry->size < sizeof (*entry) - sizeof (entry->size)
	  || entry->size > 0xffffffffU - sizeof (entry->size))
	break;
      step = entry->size + sizeof (entry->size);
      if (step > remaining)
	break;
      if (hook (entry->addr, entry->len, entry->type, hook_data))
	break;

      remaining -= step;
      entry = (void *) ((grub_addr_t) entry + step);
    }

  return 0;
}
