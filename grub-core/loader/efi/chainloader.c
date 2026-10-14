/* chainloader.c - boot another boot loader */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2002,2004,2006,2007,2008  Free Software Foundation, Inc.
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

/* TODO: support load options.  */

#include <grub/loader.h>
#include <grub/file.h>
#include <grub/err.h>
#include <grub/device.h>
#include <grub/disk.h>
#include <grub/misc.h>
#include <grub/charset.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/dl.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/efi/disk.h>
#include <grub/efi/memory.h>
#include <grub/efi/pe32.h>
#include <grub/efi/sb.h>
#include <grub/command.h>
#include <grub/i18n.h>
#include <grub/net.h>
#include <grub/script_sh.h>
#include <grub/term.h>
#if defined (__i386__) || defined (__x86_64__)
#include <grub/macho.h>
#include <grub/i386/macho.h>
#endif

GRUB_MOD_LICENSE ("GPLv3+");

static grub_dl_t my_mod;
static grub_efi_physical_address_t chainloader_address;
static grub_efi_uintn_t chainloader_pages;
static grub_efi_device_path_t *chainloader_file_path;

/* State for the alternative (manual PE) secure boot chainloader.  */
static grub_ssize_t chainloader_alt_size;
static grub_efi_char16_t *chainloader_alt_cmdline;
static grub_ssize_t chainloader_alt_cmdline_len;
static grub_efi_handle_t chainloader_alt_dev_handle;

#ifndef GRUB_PE32_SCN_CNT_UNINITIALIZED_DATA
#define GRUB_PE32_SCN_CNT_UNINITIALIZED_DATA	0x00000080
#endif

static void
grub_chainloader_debug_load_options (const char *scope,
				     grub_efi_loaded_image_t *loaded_image)
{
  grub_size_t i;
  grub_size_t n;
  grub_size_t max_chars = 255;
  char buf[256];
  grub_efi_char16_t *opt;

  if (!loaded_image || !loaded_image->load_options || !loaded_image->load_options_size)
    {
      grub_dprintf ("chain", "%s: no load options\n", scope);
      return;
    }

  opt = (grub_efi_char16_t *) loaded_image->load_options;
  n = loaded_image->load_options_size / sizeof (grub_efi_char16_t);
  if (n > max_chars)
    n = max_chars;

  for (i = 0; i < n; i++)
    {
      grub_uint16_t ch = opt[i];
      if (ch == 0)
	break;
      if (ch >= ' ' && ch <= '~')
	buf[i] = (char) ch;
      else
	buf[i] = '?';
    }
  buf[i] = '\0';

  grub_dprintf ("chain", "%s: load_options_ptr=%p load_options_size=%lu text=\"%s\"\n",
		scope,
		loaded_image->load_options,
		(unsigned long) loaded_image->load_options_size,
		buf);
}

static grub_err_t
grub_chainloader_unload (void *context)
{
  grub_efi_handle_t image_handle = (grub_efi_handle_t) context;
  grub_efi_loaded_image_t *loaded_image;
  grub_efi_boot_services_t *b;

  grub_dprintf ("chain", "chainloader unload image_handle=%p\n", image_handle);
  b = grub_efi_system_table->boot_services;
  loaded_image = grub_efi_get_loaded_image (image_handle);
  if (loaded_image != NULL)
    grub_free (loaded_image->load_options);

  if (chainloader_file_path)
    {
      grub_free (chainloader_file_path);
      chainloader_file_path = NULL;
    }
  if (chainloader_address)
    {
      b->free_pages (chainloader_address, chainloader_pages);
      chainloader_address = 0;
      chainloader_pages = 0;
    }

  grub_efi_unload_image (image_handle);

  grub_dl_unref (my_mod);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_chainloader_boot (void *context)
{
  grub_efi_handle_t image_handle = (grub_efi_handle_t) context;
  grub_efi_boot_services_t *b;
  grub_efi_status_t status;
  grub_efi_uintn_t exit_data_size;
  grub_efi_char16_t *exit_data = NULL;
  grub_efi_loaded_image_t *loaded_image = NULL;

  b = grub_efi_system_table->boot_services;
  loaded_image = grub_efi_get_loaded_image (image_handle);
  grub_chainloader_debug_load_options ("StartImage", loaded_image);
  grub_dprintf ("chain", "StartImage image_handle=%p\n", image_handle);
  status = grub_efi_start_image (image_handle, &exit_data_size, &exit_data);
  grub_dprintf ("chain", "StartImage status=0x%lx exit_data_size=%lu\n",
		(unsigned long) status, (unsigned long) exit_data_size);
  if (status != GRUB_EFI_SUCCESS)
    {
      if (exit_data)
	{
	  char *buf;

	  buf = grub_malloc (exit_data_size * 4 + 1);
	  if (buf)
	    {
	      *grub_utf16_to_utf8 ((grub_uint8_t *) buf,
				   exit_data, exit_data_size) = 0;

	      grub_error (GRUB_ERR_BAD_OS, "%s", buf);
	      grub_free (buf);
	    }
	}
      else
	grub_error (GRUB_ERR_BAD_OS, "unknown error (StartImage status=0x%lx)",
		    (unsigned long) status);
    }

  if (exit_data)
    b->free_pool (exit_data);

  grub_loader_unset ();

  return grub_errno;
}

static grub_err_t
copy_file_path (grub_efi_file_path_device_path_t *fp,
		const char *str, grub_efi_uint16_t len)
{
  grub_efi_char16_t *p, *path_name;
  grub_efi_uint16_t size;

  fp->header.type = GRUB_EFI_MEDIA_DEVICE_PATH_TYPE;
  fp->header.subtype = GRUB_EFI_FILE_PATH_DEVICE_PATH_SUBTYPE;

  path_name = grub_calloc (len, GRUB_MAX_UTF16_PER_UTF8 * sizeof (*path_name));
  if (!path_name)
    return grub_error (GRUB_ERR_OUT_OF_MEMORY, "failed to allocate path buffer");

  size = grub_utf8_to_utf16 (path_name, len * GRUB_MAX_UTF16_PER_UTF8,
			     (const grub_uint8_t *) str, len, 0);
  for (p = path_name; p < path_name + size; p++)
    if (*p == '/')
      *p = '\\';

  grub_memcpy (fp->path_name, path_name, size * sizeof (*fp->path_name));
  /* File Path is NULL terminated */
  fp->path_name[size++] = '\0';
  fp->header.length = size * sizeof (grub_efi_char16_t) + sizeof (*fp);
  grub_free (path_name);
  return GRUB_ERR_NONE;
}

static grub_efi_device_path_t *
make_file_path (grub_efi_device_path_t *dp, const char *filename)
{
  char *dir_start;
  char *dir_end;
  grub_size_t size;
  grub_efi_device_path_t *d, *file_path;

  dir_start = grub_strchr (filename, ')');
  if (! dir_start)
    dir_start = (char *) filename;
  else
    dir_start++;

  dir_end = grub_strrchr (dir_start, '/');
  if (! dir_end)
    {
      grub_error (GRUB_ERR_BAD_FILENAME, "invalid EFI file path");
      return 0;
    }

  size = 0;
  d = dp;
  while (d)
    {
      grub_size_t len = GRUB_EFI_DEVICE_PATH_LENGTH (d);

      if (len < 4)
	{
	  grub_error (GRUB_ERR_OUT_OF_RANGE,
		      "malformed EFI Device Path node has length=%" PRIuGRUB_SIZE, len);
	  return NULL;
	}

      size += len;
      if ((GRUB_EFI_END_ENTIRE_DEVICE_PATH (d)))
	break;
      d = GRUB_EFI_NEXT_DEVICE_PATH (d);
    }

  /* File Path is NULL terminated. Allocate space for 2 extra characters */
  /* FIXME why we split path in two components? */
  file_path = grub_malloc (size
			   + ((grub_strlen (dir_start) + 2)
			      * GRUB_MAX_UTF16_PER_UTF8
			      * sizeof (grub_efi_char16_t))
			   + sizeof (grub_efi_file_path_device_path_t) * 2);
  if (! file_path)
    return 0;

  grub_memcpy (file_path, dp, size);

  /* Fill the file path for the directory.  */
  d = (grub_efi_device_path_t *) ((char *) file_path
				  + ((char *) d - (char *) dp));
  if (copy_file_path ((grub_efi_file_path_device_path_t *) d,
		      dir_start, dir_end - dir_start) != GRUB_ERR_NONE)
    {
 fail:
      grub_free (file_path);
      return 0;
    }

  /* Fill the file path for the file.  */
  d = GRUB_EFI_NEXT_DEVICE_PATH (d);
  if (copy_file_path ((grub_efi_file_path_device_path_t *) d,
		      dir_end + 1, grub_strlen (dir_end + 1)) != GRUB_ERR_NONE)
    goto fail;

  /* Fill the end of device path nodes.  */
  d = GRUB_EFI_NEXT_DEVICE_PATH (d);
  d->type = GRUB_EFI_END_DEVICE_PATH_TYPE;
  d->subtype = GRUB_EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE;
  d->length = sizeof (*d);

  return file_path;
}

/*
 * Alternative secure boot chainloader.
 *
 * Firmware LoadImage refuses unsigned images under secure boot.  The a1ive
 * alternative loader maps the PE image itself (sections + base relocations)
 * and calls its entry point directly, exactly like a1ive/grub's handle_image.
 */

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#endif

struct chainloader_pe_ctx
{
  grub_efi_uint64_t image_address;
  grub_efi_uint64_t image_size;
  grub_efi_uint64_t entry_point;
  grub_efi_uintn_t size_of_headers;
  grub_efi_uint16_t image_type;
  grub_efi_uint16_t number_of_sections;
  grub_efi_uint32_t section_alignment;
  struct grub_pe32_section_table *first_section;
  struct grub_pe32_data_directory *reloc_dir;
  struct grub_pe32_data_directory *sec_dir;
  grub_efi_uint64_t number_of_rva_and_sizes;
  struct grub_pe_image_header *pe_hdr;
};
typedef struct chainloader_pe_ctx chainloader_pe_ctx_t;

#if !defined (__i386__) && !defined (__x86_64__)
struct grub_efi_shim_lock
{
  grub_efi_status_t (*verify) (void *buffer, grub_efi_uint32_t size);
  grub_efi_status_t (*hash) (void *data, grub_efi_int32_t datasize,
			     chainloader_pe_ctx_t *context,
			     grub_efi_uint8_t *sha256hash,
			     grub_efi_uint8_t *sha1hash);
  grub_efi_status_t (*context) (void *data, grub_efi_uint32_t size,
				chainloader_pe_ctx_t *context);
};
typedef struct grub_efi_shim_lock grub_efi_shim_lock_t;
#endif

static grub_efi_boolean_t
chainloader_read_header (void *data, grub_efi_uint32_t size,
			 chainloader_pe_ctx_t *context)
{
#if defined (__i386__) || defined (__x86_64__)
  char *msdos = (char *) data;
  struct grub_pe_image_header *pe_hdr = (struct grub_pe_image_header *) data;

  if (size < sizeof (*pe_hdr))
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "Invalid image");
      return 0;
    }

  if (grub_memcmp (msdos, "MZ", 2) == 0)
    {
      grub_uint32_t off = *((grub_uint32_t *) (msdos + 0x3c));
      if (off > size || size - off < sizeof (*pe_hdr))
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT, "Invalid image");
	  return 0;
	}
      pe_hdr = (struct grub_pe_image_header *) ((char *) data + off);
    }

  if (grub_memcmp (pe_hdr->signature, "PE\0\0", 4) != 0 ||
#if defined (__x86_64__)
      pe_hdr->coff_header.machine != GRUB_PE32_MACHINE_X86_64 ||
      pe_hdr->optional_header.magic != GRUB_PE32_PE64_MAGIC
#else
      pe_hdr->coff_header.machine != GRUB_PE32_MACHINE_I386 ||
      pe_hdr->optional_header.magic != GRUB_PE32_PE32_MAGIC
#endif
      )
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "Not supported image");
      return 0;
    }

  context->number_of_rva_and_sizes
    = pe_hdr->optional_header.num_data_directories;
  context->size_of_headers = pe_hdr->optional_header.header_size;
  context->image_size = pe_hdr->optional_header.image_size;
  context->image_address = pe_hdr->optional_header.image_base;
  context->entry_point = pe_hdr->optional_header.entry_addr;
  context->reloc_dir = &pe_hdr->optional_header.base_relocation_table;
  context->number_of_sections = pe_hdr->coff_header.num_sections;
  context->section_alignment = pe_hdr->optional_header.section_alignment;
  context->first_section
    = (struct grub_pe32_section_table *)
      ((char *) &pe_hdr->optional_header
       + pe_hdr->coff_header.optional_header_size);
  context->pe_hdr = pe_hdr;
  return 1;
#else
  grub_efi_guid_t guid = GRUB_EFI_SHIM_LOCK_GUID;
  grub_efi_shim_lock_t *shim_lock;
  grub_efi_status_t status;

  shim_lock = grub_efi_locate_protocol (&guid, NULL);
  if (! shim_lock)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "no shim lock protocol");
      return 0;
    }

  status = shim_lock->context (data, size, context);
  if (status == GRUB_EFI_SUCCESS)
    return 1;

  grub_error (GRUB_ERR_BAD_ARGUMENT, "shim context error code 0x%lx",
	      (unsigned long) status);
  return 0;
#endif
}

static void *
chainloader_image_address (void *image, grub_efi_uint64_t sz,
			   grub_efi_uint64_t adr)
{
  if (adr >= sz)
    return NULL;

  return ((grub_uint8_t *) image + adr);
}

static grub_efi_status_t
chainloader_relocate_coff (chainloader_pe_ctx_t *context,
			   struct grub_pe32_section_table *section,
			   void *orig, grub_efi_uint32_t datasize, void *data)
{
  struct grub_pe32_data_directory *reloc_base, *reloc_base_end;
  grub_efi_uint64_t adjust;
  struct grub_pe32_fixup_block *reloc, *reloc_end;
  char *fixup, *fixup_base;
  grub_efi_uint16_t *fixup_16;
  grub_efi_uint32_t *fixup_32;
  grub_efi_uint64_t *fixup_64;
  grub_efi_uint64_t size = context->image_size;
  void *file_end = (char *) orig + datasize;
  int n = 0;

  context->pe_hdr->optional_header.image_base = (grub_addr_t) data;

  if (section->raw_data_offset > datasize
      || context->reloc_dir->size
         > datasize - section->raw_data_offset)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "Reloc table overflows source file");
      return GRUB_EFI_UNSUPPORTED;
    }
  reloc_base = (struct grub_pe32_data_directory *)
    ((char *) orig + section->raw_data_offset);
  reloc_base_end = (struct grub_pe32_data_directory *)
    ((char *) reloc_base + context->reloc_dir->size);

  if (! reloc_base && ! reloc_base_end)
    return GRUB_EFI_SUCCESS;

  if (! reloc_base || ! reloc_base_end)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "Reloc table overflows binary");
      return GRUB_EFI_UNSUPPORTED;
    }

  adjust = (grub_efi_uint64_t) (grub_efi_uintn_t) data
    - context->image_address;
  if (adjust == 0)
    return GRUB_EFI_SUCCESS;

  while (reloc_base < reloc_base_end)
    {
      grub_uint16_t *entry;
      reloc = (struct grub_pe32_fixup_block *) ((char *) reloc_base);

      if ((char *) reloc_base_end - (char *) reloc_base
          < (grub_ssize_t) sizeof (*reloc)
	  || reloc_base->size < sizeof (*reloc)
	  || reloc_base->size > (grub_size_t)
	     ((char *) reloc_base_end - (char *) reloc_base)
	  || (reloc_base->size - sizeof (*reloc))
	     % sizeof (grub_uint16_t))
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT,
		      "Reloc %d block size %d is invalid", n,
		      reloc_base->size);
	  return GRUB_EFI_UNSUPPORTED;
	}

      entry = &reloc->entries[0];
      reloc_end = (struct grub_pe32_fixup_block *)
	((char *) reloc_base + reloc_base->size);

      if ((void *) reloc_end < orig || (void *) reloc_end > file_end)
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT,
		      "Reloc entry %d overflows binary", n);
	  return GRUB_EFI_UNSUPPORTED;
	}

      fixup_base = chainloader_image_address (data, size, reloc_base->rva);
      if (! fixup_base)
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT, "Reloc %d Invalid fixupbase", n);
	  return GRUB_EFI_UNSUPPORTED;
	}

      while ((void *) entry < (void *) reloc_end)
	{
	  grub_efi_uint64_t target_rva =
	    (grub_efi_uint64_t) reloc_base->rva + (*entry & 0xfff);
	  grub_efi_uintn_t width = 0;

	  switch ((*entry) >> 12)
	    {
	    case GRUB_PE32_REL_BASED_ABSOLUTE:
	      break;
	    case GRUB_PE32_REL_BASED_HIGH:
	    case GRUB_PE32_REL_BASED_LOW:
	      width = sizeof (grub_uint16_t);
	      break;
	    case GRUB_PE32_REL_BASED_HIGHLOW:
	      width = sizeof (grub_uint32_t);
	      break;
	    case GRUB_PE32_REL_BASED_DIR64:
	      width = sizeof (grub_uint64_t);
	      break;
	    default:
	      grub_error (GRUB_ERR_BAD_ARGUMENT, "unknown PE relocation type");
	      return GRUB_EFI_UNSUPPORTED;
	    }
	  if (width && (target_rva >= size || width > size - target_rva))
	    {
	      grub_error (GRUB_ERR_BAD_ARGUMENT, "PE relocation target out of bounds");
	      return GRUB_EFI_UNSUPPORTED;
	    }
	  fixup = fixup_base + (*entry & 0xFFF);
	  switch ((*entry) >> 12)
	    {
	    case GRUB_PE32_REL_BASED_ABSOLUTE:
	      break;
	    case GRUB_PE32_REL_BASED_HIGH:
	      fixup_16 = (grub_efi_uint16_t *) fixup;
	      *fixup_16 = (grub_efi_uint16_t)
		(*fixup_16 + ((grub_efi_uint16_t) ((grub_efi_uint32_t) adjust >> 16)));
	      break;
	    case GRUB_PE32_REL_BASED_LOW:
	      fixup_16 = (grub_efi_uint16_t *) fixup;
	      *fixup_16 = (grub_efi_uint16_t) (*fixup_16 + (grub_efi_uint16_t) adjust);
	      break;
	    case GRUB_PE32_REL_BASED_HIGHLOW:
	      fixup_32 = (grub_efi_uint32_t *) fixup;
	      *fixup_32 = *fixup_32 + (grub_efi_uint32_t) adjust;
	      break;
	    case GRUB_PE32_REL_BASED_DIR64:
	      fixup_64 = (grub_efi_uint64_t *) fixup;
	      *fixup_64 = *fixup_64 + (grub_efi_uint64_t) adjust;
	      break;
	    default:
	      grub_error (GRUB_ERR_BAD_ARGUMENT,
			  "Reloc %d unknown relocation type %d",
			  n, (*entry) >> 12);
	      return GRUB_EFI_UNSUPPORTED;
	    }
	  entry += 1;
	}
      reloc_base = (struct grub_pe32_data_directory *) reloc_end;
      n++;
    }

  return GRUB_EFI_SUCCESS;
}

static grub_efi_device_path_t *
chainloader_get_media_file_path (grub_efi_device_path_t *dp)
{
  while (1)
    {
      grub_efi_uint8_t type = GRUB_EFI_DEVICE_PATH_TYPE (dp);
      grub_efi_uint8_t subtype = GRUB_EFI_DEVICE_PATH_SUBTYPE (dp);

      if (type == GRUB_EFI_END_DEVICE_PATH_TYPE)
	break;
      else if (type == GRUB_EFI_MEDIA_DEVICE_PATH_TYPE
	       && subtype == GRUB_EFI_FILE_PATH_DEVICE_PATH_SUBTYPE)
	return dp;

      dp = GRUB_EFI_NEXT_DEVICE_PATH (dp);
    }

  return NULL;
}

static grub_efi_boolean_t
chainloader_handle_image (void *data, grub_efi_uint32_t datasize)
{
  grub_efi_loaded_image_t *li, li_bak;
  grub_efi_status_t efi_status;
  grub_efi_boot_services_t *b;
  void *buffer = NULL;
  char *buffer_aligned = NULL;
  grub_efi_uint32_t i;
  struct grub_pe32_section_table *section;
  char *base, *end;
  chainloader_pe_ctx_t context;
  grub_efi_uint32_t section_alignment;
  grub_efi_uintn_t buffer_size;
  grub_addr_t section_offset;
  grub_efi_device_path_t *alt_file_path;
  int found_entry_point = 0;
  grub_efi_status_t (__grub_efi_api *entry_point)
    (grub_efi_handle_t image_handle, grub_efi_system_table_t *system_table);

  b = grub_efi_system_table->boot_services;

  if (! chainloader_read_header (data, datasize, &context))
    goto error_exit;

  /* The old manual loader trusted PE metadata.  Bound every copy by the
     source file or by SizeOfImage before touching the firmware image.  */
  if (context.image_size == 0 || context.size_of_headers == 0
      || context.size_of_headers > datasize
      || context.size_of_headers > context.image_size
      || context.entry_point >= context.image_size
#if defined (__i386__) || defined (__x86_64__)
      || context.pe_hdr->coff_header.optional_header_size
         < sizeof (context.pe_hdr->optional_header)
#endif
      || context.number_of_sections == 0)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid PE image bounds");
      goto error_exit;
    }

  if ((grub_addr_t) context.first_section < (grub_addr_t) data)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid PE section table");
      goto error_exit;
    }
  section_offset = (grub_addr_t) context.first_section - (grub_addr_t) data;
  if (section_offset > datasize
      || context.number_of_sections
         > (datasize - section_offset) / sizeof (*section))
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "truncated PE section table");
      goto error_exit;
    }

  section_alignment = context.section_alignment;
  if (section_alignment == 0)
    section_alignment = 4096;
  if ((section_alignment & (section_alignment - 1)) != 0
      || context.image_size
         > (~(grub_efi_uintn_t) 0) - section_alignment)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid PE section alignment");
      goto error_exit;
    }

  buffer_size = context.image_size + section_alignment;

  efi_status = b->allocate_pool (GRUB_EFI_LOADER_DATA, buffer_size, &buffer);
  if (efi_status != GRUB_EFI_SUCCESS)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));
      goto error_exit;
    }

  buffer_aligned = (char *) ALIGN_UP ((grub_addr_t) buffer, section_alignment);
  if (! buffer_aligned)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));
      goto error_exit;
    }

  grub_memcpy (buffer_aligned, data, context.size_of_headers);

  entry_point = chainloader_image_address (buffer_aligned, context.image_size,
					   context.entry_point);
  if (! entry_point)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid entry point");
      goto error_exit;
    }

  char *reloc_base, *reloc_base_end;
  struct grub_pe32_section_table *reloc_section = NULL;

  reloc_base = chainloader_image_address (buffer_aligned, context.image_size,
					  context.reloc_dir->rva);
  /* RelocBaseEnd here is the address of the last byte of the table */
  reloc_base_end = context.reloc_dir->size
    ? chainloader_image_address (buffer_aligned, context.image_size,
				 context.reloc_dir->rva
				 + context.reloc_dir->size - 1)
    : NULL;

  section = context.first_section;
  for (i = 0; i < context.number_of_sections; i++, section++)
    {
      char name[9];

      if (section->virtual_size == 0
          || section->virtual_address >= context.image_size
          || section->virtual_size
             > context.image_size - section->virtual_address
          || section->raw_data_size
             > context.image_size - section->virtual_address
          || (section->raw_data_size != 0
              && (section->raw_data_offset > datasize
                  || section->raw_data_size
                     > datasize - section->raw_data_offset)))
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT, "PE section %u out of bounds", i);
	  goto error_exit;
	}

      base = chainloader_image_address (buffer_aligned, context.image_size,
					section->virtual_address);
      end = chainloader_image_address (buffer_aligned, context.image_size,
				       section->virtual_address
				       + section->virtual_size - 1);

      grub_strncpy (name, section->name, 9);
      name[8] = '\0';

      if (end < base)
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT,
		      "Image has invalid negative size");
	  goto error_exit;
	}

      if (section->virtual_address <= context.entry_point &&
	  (section->virtual_address + section->raw_data_size - 1)
	  > context.entry_point)
	found_entry_point++;

      /* We do want to process .reloc, but it's often marked discardable,
       * so we don't want to memcpy it.  */
      if (grub_memcmp (section->name, ".reloc\0\0", 8) == 0)
	{
	  if (reloc_section)
	    {
	      grub_error (GRUB_ERR_BAD_ARGUMENT,
			  "Image has multiple relocation sections");
	      goto error_exit;
	    }

	  if (section->raw_data_size && section->virtual_size
	      && base && end && reloc_base == base && reloc_base_end == end)
	    reloc_section = section;
	}

      if (section->characteristics & GRUB_PE32_SCN_MEM_DISCARDABLE)
	continue;

      if (! base || ! end)
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT, "Invalid section size");
	  goto error_exit;
	}

      if (section->characteristics & GRUB_PE32_SCN_CNT_UNINITIALIZED_DATA)
	{
	  if (section->raw_data_size != 0)
	    grub_dprintf ("chain", "chain: UNINITIALIZED_DATA section has data?\n");
	}
      else if (section->virtual_address < context.size_of_headers ||
	       section->raw_data_offset < context.size_of_headers)
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT,
		      "Section %d is inside image headers", i);
	  goto error_exit;
	}

      if (section->raw_data_size > 0)
	grub_memcpy (base, (grub_uint8_t *) data + section->raw_data_offset,
		     section->raw_data_size);

      if (section->raw_data_size < section->virtual_size)
	grub_memset (base + section->raw_data_size, 0,
		     section->virtual_size - section->raw_data_size);
    }

  /* 5 == EFI_IMAGE_DIRECTORY_ENTRY_BASERELOC */
  if (context.number_of_rva_and_sizes <= 5)
    {
      grub_dprintf ("chain", "chain: image has no relocation entry\n");
      goto error_exit;
    }

  if (context.reloc_dir->size && reloc_section)
    {
      if (context.reloc_dir->size > reloc_section->raw_data_size)
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT, "truncated PE relocation table");
	  goto error_exit;
	}
      efi_status = chainloader_relocate_coff (&context, reloc_section,
					      data, datasize, buffer_aligned);
      if (efi_status != GRUB_EFI_SUCCESS)
	{
	  grub_error (GRUB_ERR_BAD_ARGUMENT, "relocation failed");
	  goto error_exit;
	}
    }

  if (! found_entry_point)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "entry point is not within sections");
      goto error_exit;
    }
  if (found_entry_point > 1)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "%d sections contain entry point",
		  found_entry_point);
      goto error_exit;
    }

  li = grub_efi_get_loaded_image (grub_efi_image_handle);
  if (! li)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "no loaded image available");
      goto error_exit;
    }

  alt_file_path = chainloader_file_path
    ? chainloader_get_media_file_path (chainloader_file_path) : NULL;
  if (! alt_file_path)
    {
      grub_error (GRUB_ERR_UNKNOWN_DEVICE, "no matching file path found");
      goto error_exit;
    }

  grub_memcpy (&li_bak, li, sizeof (grub_efi_loaded_image_t));
  li->image_base = buffer_aligned;
  li->image_size = context.image_size;
  li->load_options = chainloader_alt_cmdline;
  li->load_options_size = chainloader_alt_cmdline_len;
  li->file_path = alt_file_path;
  li->device_handle = chainloader_alt_dev_handle;

  grub_dprintf ("chain", "chain: booting via alternative entry point\n");
  efi_status = entry_point (grub_efi_image_handle, grub_efi_system_table);

  grub_memcpy (li, &li_bak, sizeof (grub_efi_loaded_image_t));
  b->free_pool (buffer);

  if (efi_status != GRUB_EFI_SUCCESS)
    {
      grub_error (GRUB_ERR_BAD_OS, "alternative image returned EFI error");
      return 0;
    }
  return 1;

 error_exit:
  if (buffer)
    b->free_pool (buffer);

  if (grub_errno)
    grub_print_error ();

  return 0;
}

static grub_err_t
grub_alt_chainloader_unload (void *context __attribute__ ((unused)))
{
  grub_efi_boot_services_t *b;

  b = grub_efi_system_table->boot_services;

  grub_free (chainloader_alt_cmdline);
  chainloader_alt_cmdline = NULL;
  chainloader_alt_cmdline_len = 0;

  if (chainloader_file_path)
    {
      grub_free (chainloader_file_path);
      chainloader_file_path = NULL;
    }
  if (chainloader_address)
    {
      b->free_pages (chainloader_address, chainloader_pages);
      chainloader_address = 0;
      chainloader_pages = 0;
    }

  grub_dl_unref (my_mod);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_alt_chainloader_boot (void *context __attribute__ ((unused)))
{
  grub_efi_boolean_t succeeded = 0;

  if (chainloader_alt_size > 0
      && (grub_uint64_t) chainloader_alt_size <= 0xffffffffULL)
    succeeded = chainloader_handle_image
      ((void *) (grub_addr_t) chainloader_address, chainloader_alt_size);
  grub_loader_unset ();
  if (! succeeded)
    return grub_error (GRUB_ERR_BAD_OS, "alternative EFI chainload failed");
  return GRUB_ERR_NONE;
}

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

static grub_err_t
grub_cmd_chainloader (grub_command_t cmd __attribute__ ((unused)),
		      int argc, char *argv[])
{
  grub_file_t file = 0;
  grub_ssize_t size;
  grub_efi_status_t status;
  grub_efi_boot_services_t *b;
  grub_device_t dev = 0;
  grub_efi_device_path_t *dp = NULL, *file_path = NULL;
  grub_efi_loaded_image_t *loaded_image;
  char *filename;
  void *boot_image = 0;
  grub_efi_handle_t dev_handle = 0;
  grub_efi_physical_address_t address = 0;
  grub_efi_uintn_t pages = 0;
  grub_efi_char16_t *cmdline = NULL;
  grub_ssize_t cmdline_len = 0;
  grub_efi_handle_t image_handle = NULL;
  int fallback_dev_opened = 0;
  int text_mode = 0;
  int quiet = 0;
  int boot_now = 0;
  int alt_mode = 0;

  while (argc > 0)
    {
      if (grub_strcmp (argv[0], "-t") == 0 || grub_strcmp (argv[0], "--text") == 0)
	{
	  text_mode = 1;
	  argc--;
	  argv++;
	  continue;
	}
      if (grub_strcmp (argv[0], "-a") == 0 || grub_strcmp (argv[0], "--alt") == 0)
	{
	  alt_mode = 1;
	  argc--;
	  argv++;
	  continue;
	}
      if (grub_strcmp (argv[0], "-q") == 0 || grub_strcmp (argv[0], "--quiet") == 0)
	{
	  quiet = 1;
	  argc--;
	  argv++;
	  continue;
	}
      if (grub_strcmp (argv[0], "-b") == 0 || grub_strcmp (argv[0], "--boot") == 0)
	{
	  boot_now = 1;
	  argc--;
	  argv++;
	  continue;
	}
      if (grub_strcmp (argv[0], "--") == 0)
	{
	  argc--;
	  argv++;
	  break;
	}
      break;
    }

  if (argc == 0)
    {
      if (quiet)
	return GRUB_ERR_NONE;
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
    }
  filename = argv[0];
  grub_dprintf ("chain", "chainloader argc=%d filename=%s\n", argc, filename);

  if (text_mode)
    {
      grub_script_execute_sourcecode ("terminal_output console");
      if (!quiet)
	{
	  grub_printf ("Switch to text mode.\n");
	  grub_refresh ();
	}
    }

  grub_dl_ref (my_mod);
  chainloader_address = 0;
  chainloader_pages = 0;
  chainloader_file_path = NULL;

  b = grub_efi_system_table->boot_services;

  file = grub_file_open (filename, GRUB_FILE_TYPE_EFI_CHAINLOADED_IMAGE);
  if (! file)
    goto fail;

  /*
   * Build device path from the actual source device of FILE.
   * Using current root may produce a mismatched device path when chainloading
   * an image from a different device than $root.
   */
  dev = file->device;
  if (dev == NULL)
    {
      dev = grub_device_open (0);
      fallback_dev_opened = 1;
    }

  if (dev == NULL)
    ;
  else if (dev->disk)
    dev_handle = grub_efidisk_get_device_handle (dev->disk);
  else if (dev->net && dev->net->server)
    {
      grub_net_network_level_address_t addr;
      struct grub_net_network_level_interface *inf;
      grub_net_network_level_address_t gateway;
      grub_err_t err;

      err = grub_net_resolve_address (dev->net->server, &addr);
      if (err)
	goto fail;

      err = grub_net_route_address (addr, &gateway, &inf);
      if (err)
	goto fail;

      dev_handle = grub_efinet_get_device_handle (inf->card);
    }

  if (dev_handle)
    dp = grub_efi_get_device_path (dev_handle);
  grub_dprintf ("chain", "device_handle=%p device_path=%p\n", dev_handle, dp);

  if (dp != NULL)
    {
      file_path = make_file_path (dp, filename);
      if (file_path == NULL)
	goto fail;
      grub_dprintf ("chain", "built file_path=%p for %s\n", file_path, filename);
    }

  size = grub_file_size (file);
  if (!size)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		  filename);
      goto fail;
    }
  pages = (grub_efi_uintn_t) GRUB_EFI_BYTES_TO_PAGES (size);
  grub_dprintf ("chain", "file_size=%ld pages=%lu\n", (long) size,
		(unsigned long) pages);

  status = b->allocate_pages (GRUB_EFI_ALLOCATE_ANY_PAGES,
			      GRUB_EFI_LOADER_CODE,
			      pages, &address);
  if (status != GRUB_EFI_SUCCESS)
    {
      grub_dprintf ("chain", "Failed to allocate %u pages\n",
		    (unsigned int) pages);
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));
      goto fail;
    }

  boot_image = (void *) ((grub_addr_t) address);
  grub_dprintf ("chain", "allocated boot_image=%p\n", boot_image);
  if (grub_file_read (file, boot_image, size) != size)
    {
      if (grub_errno == GRUB_ERR_NONE)
	grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		    filename);

      goto fail;
    }

#if defined (__i386__) || defined (__x86_64__)
  if (size >= (grub_ssize_t) sizeof (struct grub_macho_fat_header))
    {
      struct grub_macho_fat_header *head = boot_image;
      if (head->magic
	  == grub_cpu_to_le32_compile_time (GRUB_MACHO_FAT_EFI_MAGIC))
	{
	  grub_uint32_t i;
	  struct grub_macho_fat_arch *archs
	    = (struct grub_macho_fat_arch *) (head + 1);
	  for (i = 0; i < grub_cpu_to_le32 (head->nfat_arch); i++)
	    {
	      if (GRUB_MACHO_CPUTYPE_IS_HOST_CURRENT (archs[i].cputype))
		break;
	    }
	  if (i == grub_cpu_to_le32 (head->nfat_arch))
	    {
	      grub_error (GRUB_ERR_BAD_OS, "no compatible arch found");
	      goto fail;
	    }
	  if (grub_cpu_to_le32 (archs[i].offset)
	      > ~grub_cpu_to_le32 (archs[i].size)
	      || grub_cpu_to_le32 (archs[i].offset)
	      + grub_cpu_to_le32 (archs[i].size)
	      > (grub_size_t) size)
	    {
	      grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
			  filename);
	      goto fail;
	    }
	  boot_image = (char *) boot_image + grub_cpu_to_le32 (archs[i].offset);
	  size = grub_cpu_to_le32 (archs[i].size);
	  grub_dprintf ("chain", "selected Mach-O arch=%lu offset=%lu size=%ld\n",
			(unsigned long) i,
			(unsigned long) grub_cpu_to_le32 (archs[i].offset),
			(long) size);
	}
    }
#endif

  /* Build load options with arguments from chainloader command line. */
  if (argc > 1)
    {
      int i, len;
      grub_efi_char16_t *p16;

      for (i = 1, len = 0; i < argc; i++)
        len += grub_strlen (argv[i]) + 1;

      len *= sizeof (grub_efi_char16_t);
      cmdline = p16 = grub_malloc (len);
      if (! cmdline)
        goto fail;

      for (i = 1; i < argc; i++)
        {
          char *p8;

          p8 = argv[i];
          while (*p8)
            *(p16++) = *(p8++);

          *(p16++) = ' ';
        }
      *(--p16) = 0;

      cmdline_len = len;
      grub_dprintf ("chain", "load_options_size=%lu\n", (unsigned long) len);
    }

  if (alt_mode)
    goto alt_setup;

  image_handle = grub_efi_get_last_verified_image_handle ();
  if (image_handle != NULL)
    grub_dprintf ("chain", "using verified image_handle=%p\n", image_handle);
  if (image_handle == NULL)
    {
      grub_dprintf ("chain", "LoadImage from memory file_path=%p size=%ld\n",
		    file_path, (long) size);
      status = grub_efi_load_image (0, grub_efi_image_handle, file_path,
				boot_image, size, &image_handle);
      grub_dprintf ("chain", "LoadImage status=0x%lx image_handle=%p\n",
		    (unsigned long) status, image_handle);
      if (status == GRUB_EFI_SECURITY_VIOLATION)
	{
	  /* Firmware refused the image (secure boot).  Fall back to the
	     alternative loader the same way a1ive does.  */
	  grub_dprintf ("chain", "LoadImage failed with EFI_SECURITY_VIOLATION; "
			"try alternative chainloader\n");
	  goto alt_setup;
	}
      if (status != GRUB_EFI_SUCCESS)
	{
	  if (status == GRUB_EFI_OUT_OF_RESOURCES)
	    grub_error (GRUB_ERR_OUT_OF_MEMORY, "out of resources");
	  else
	    grub_error (GRUB_ERR_BAD_OS, "cannot load image");

	  goto fail;
	}
    }

  /* LoadImage does not set a device handler when the image is
     loaded from memory, so it is necessary to set it explicitly here.
     This is a mess.  */
  loaded_image = grub_efi_get_loaded_image (image_handle);
  if (! loaded_image)
    {
      grub_error (GRUB_ERR_BAD_OS, "no loaded image available");
      goto fail;
    }
  loaded_image->device_handle = dev_handle;
  grub_dprintf ("chain", "loaded_image=%p device_handle=%p\n",
		loaded_image, dev_handle);

  if (cmdline)
    {
      loaded_image->load_options = cmdline;
      loaded_image->load_options_size = cmdline_len;
      grub_chainloader_debug_load_options ("chainloader", loaded_image);
    }

  grub_file_close (file);
  if (fallback_dev_opened && dev)
    grub_device_close (dev);

  /*
   * Keep source buffer and device path alive until unload.
   * Some firmware implementations may still reference them around StartImage.
   */
  chainloader_address = address;
  chainloader_pages = pages;
  chainloader_file_path = file_path;
  address = 0;
  pages = 0;
  file_path = NULL;

  grub_loader_set_ex (grub_chainloader_boot, grub_chainloader_unload, image_handle, 0);
  grub_dprintf ("chain", "chainloader setup complete image_handle=%p\n",
		image_handle);
  if (boot_now)
    return grub_chainloader_boot (image_handle);
  return 0;

 alt_setup:
  /*
   * Alternative secure boot path: hand the raw image and the source
   * device path over to the manual PE loader instead of LoadImage.
   */
  grub_file_close (file);
  if (fallback_dev_opened && dev)
    grub_device_close (dev);

  chainloader_alt_size = size;
  chainloader_alt_cmdline = cmdline;
  chainloader_alt_cmdline_len = cmdline_len;
  chainloader_alt_dev_handle = dev_handle;
  chainloader_address = address;
  chainloader_pages = pages;
  chainloader_file_path = file_path;
  address = 0;
  pages = 0;
  file_path = NULL;
  cmdline = NULL;

  grub_loader_set_ex (grub_alt_chainloader_boot, grub_alt_chainloader_unload,
		      NULL, 0);
  grub_dprintf ("chain", "alternative chainloader setup complete size=%ld\n",
		(long) size);
  if (boot_now)
    return grub_alt_chainloader_boot (NULL);
  return 0;

 fail:
  grub_dprintf ("chain", "chainloader failed filename=%s grub_errno=%d\n",
		filename ? filename : "(null)", grub_errno);

  if (fallback_dev_opened && dev)
    grub_device_close (dev);

  if (file)
    grub_file_close (file);

  grub_free (cmdline);
  grub_free (file_path);

  if (address)
    b->free_pages (address, pages);

  if (image_handle != NULL)
    grub_efi_unload_image (image_handle);

  grub_dl_unref (my_mod);

  if (quiet)
    {
      grub_errno = GRUB_ERR_NONE;
      return GRUB_ERR_NONE;
    }

  return grub_errno;
}

static grub_command_t cmd;

GRUB_MOD_INIT(chainloader)
{
  cmd = grub_register_command ("chainloader", grub_cmd_chainloader,
			       0, N_("Load another boot loader."));
  my_mod = mod;
}

GRUB_MOD_FINI(chainloader)
{
  grub_unregister_command (cmd);
}
