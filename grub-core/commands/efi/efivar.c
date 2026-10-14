/*
 * efivar.c - simple EFI variable get/set/delete command
 */

#include <grub/types.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/command.h>
#include <grub/extcmd.h>
#include <grub/err.h>
#include <grub/i18n.h>
#include <grub/charset.h>
#include <grub/env.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>

GRUB_MOD_LICENSE ("GPLv3+");

static grub_command_t cmd_efivar;
static grub_extcmd_t cmd_getenv;
static grub_extcmd_t cmd_setenv;
static grub_extcmd_t cmd_lsefienv;

static int
hex_nibble (char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  c = grub_tolower (c);
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

static int
parse_hex_value (const char *s, grub_size_t n, grub_uint64_t *out)
{
  grub_uint64_t v = 0;
  grub_size_t i;

  for (i = 0; i < n; i++)
    {
      int x = hex_nibble (s[i]);
      if (x < 0)
        return 0;
      v = (v << 4) | (grub_uint64_t) x;
    }
  *out = v;
  return 1;
}

static int
parse_guid (const char *s, grub_guid_t *guid)
{
  grub_uint64_t v;
  grub_size_t len = grub_strlen (s);

  if (len != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
    return 0;

  if (!parse_hex_value (s, 8, &v))
    return 0;
  guid->data1 = (grub_uint32_t) v;

  if (!parse_hex_value (s + 9, 4, &v))
    return 0;
  guid->data2 = (grub_uint16_t) v;

  if (!parse_hex_value (s + 14, 4, &v))
    return 0;
  guid->data3 = (grub_uint16_t) v;

  if (!parse_hex_value (s + 19, 2, &v))
    return 0;
  guid->data4[0] = (grub_uint8_t) v;
  if (!parse_hex_value (s + 21, 2, &v))
    return 0;
  guid->data4[1] = (grub_uint8_t) v;

  if (!parse_hex_value (s + 24, 2, &v))
    return 0;
  guid->data4[2] = (grub_uint8_t) v;
  if (!parse_hex_value (s + 26, 2, &v))
    return 0;
  guid->data4[3] = (grub_uint8_t) v;
  if (!parse_hex_value (s + 28, 2, &v))
    return 0;
  guid->data4[4] = (grub_uint8_t) v;
  if (!parse_hex_value (s + 30, 2, &v))
    return 0;
  guid->data4[5] = (grub_uint8_t) v;
  if (!parse_hex_value (s + 32, 2, &v))
    return 0;
  guid->data4[6] = (grub_uint8_t) v;
  if (!parse_hex_value (s + 34, 2, &v))
    return 0;
  guid->data4[7] = (grub_uint8_t) v;

  return 1;
}

enum efivar_value_type
{
  EFIVAR_TYPE_STRING = 0,
  EFIVAR_TYPE_WSTRING,
  EFIVAR_TYPE_UINT8,
  EFIVAR_TYPE_HEX
};

static const struct grub_arg_option efienv_options[] =
  {
    {"guid", 'g', 0, N_("EFI variable GUID."), N_("GUID"), ARG_TYPE_STRING},
    {"type", 't', 0, N_("Value type: string/wstring/uint8/hex."),
     N_("TYPE"), ARG_TYPE_STRING},
    {0, 0, 0, 0, 0, 0}
  };

static const struct grub_arg_option efienv_set_options[] =
  {
    {"guid", 'g', 0, N_("EFI variable GUID."), N_("GUID"), ARG_TYPE_STRING},
    {"type", 't', 0, N_("Value type: string/wstring/uint8/hex."),
     N_("TYPE"), ARG_TYPE_STRING},
    {"nv", 'n', 0, N_("Store the EFI variable as non-volatile."), 0, 0},
    {0, 0, 0, 0, 0, 0}
  };

static const struct grub_arg_option efienv_list_options[] =
  {
    {"guid", 'g', 0, N_("EFI variable GUID."), N_("GUID"), ARG_TYPE_STRING},
    {0, 0, 0, 0, 0, 0}
  };

static int
parse_type (const char *s, enum efivar_value_type *type)
{
  if (!s || grub_strcmp (s, "hex") == 0)
    {
      *type = EFIVAR_TYPE_HEX;
      return 1;
    }
  if (grub_strcmp (s, "string") == 0)
    {
      *type = EFIVAR_TYPE_STRING;
      return 1;
    }
  if (grub_strcmp (s, "wstring") == 0)
    {
      *type = EFIVAR_TYPE_WSTRING;
      return 1;
    }
  if (grub_strcmp (s, "uint8") == 0)
    {
      *type = EFIVAR_TYPE_UINT8;
      return 1;
    }
  return 0;
}

static grub_err_t
get_guid_from_args (grub_extcmd_context_t ctxt, grub_guid_t *guid)
{
  static const grub_guid_t global = GRUB_EFI_GLOBAL_VARIABLE_GUID;

  *guid = global;
  if (ctxt->state[0].set)
    {
      if (!parse_guid (ctxt->state[0].arg, guid))
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid GUID format");
    }
  return GRUB_ERR_NONE;
}

static void
print_hex_data (const grub_uint8_t *data, grub_size_t len)
{
  grub_size_t i;

  for (i = 0; i < len; i++)
    grub_printf ("%02x", data[i]);
  grub_printf ("\n");
}

static char *
hex_data_to_string (const grub_uint8_t *data, grub_size_t len)
{
  static const char hex[] = "0123456789abcdef";
  char *out;
  grub_size_t i;

  out = grub_malloc (len * 2 + 1);
  if (!out)
    return NULL;

  for (i = 0; i < len; i++)
    {
      out[2 * i] = hex[data[i] >> 4];
      out[2 * i + 1] = hex[data[i] & 0x0f];
    }
  out[2 * len] = '\0';
  return out;
}

static char *
efi_data_to_utf8 (const void *data, grub_size_t datasz)
{
  const grub_uint16_t *u16 = data;
  grub_size_t len16 = datasz / 2;
  grub_size_t outsz;
  char *out;
  grub_uint8_t *end;

  if (datasz < 2 || (datasz & 1) != 0)
    return NULL;

  /* Strip trailing NUL if present.  */
  while (len16 > 0 && u16[len16 - 1] == 0)
    len16--;

  outsz = len16 * GRUB_MAX_UTF8_PER_UTF16 + 1;
  out = grub_malloc (outsz);
  if (!out)
    return NULL;

  end = grub_utf16_to_utf8 ((grub_uint8_t *) out, u16, len16);
  *end = '\0';
  return out;
}

static int
guid_equal (const grub_guid_t *a, const grub_guid_t *b)
{
  return (grub_memcmp (a, b, sizeof (*a)) == 0);
}

static char *
utf16_name_to_utf8 (const grub_efi_char16_t *name16)
{
  grub_size_t len16 = 0;
  grub_size_t buflen;
  char *name8;
  grub_uint8_t *end;

  while (name16[len16])
    len16++;

  buflen = len16 * GRUB_MAX_UTF8_PER_UTF16 + 1;
  name8 = grub_malloc (buflen);
  if (!name8)
    return NULL;

  end = grub_utf16_to_utf8 ((grub_uint8_t *) name8, (const grub_uint16_t *) name16, len16);
  *end = '\0';
  return name8;
}

static grub_err_t
handle_list (int argc, char **argv)
{
  grub_efi_runtime_services_t *r = grub_efi_system_table->runtime_services;
  grub_efi_status_t st;
  grub_guid_t guid = {0};
  grub_guid_t filter_guid = {0};
  int use_filter = 0;
  grub_size_t cap = 128 * sizeof (grub_efi_char16_t);
  grub_efi_char16_t *name = grub_zalloc (cap);
  grub_efi_uintn_t name_size;

  if (argc != 1 && argc != 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "usage: efivar list [GUID]");

  if (argc == 2)
    {
      if (!parse_guid (argv[1], &filter_guid))
        {
          grub_free (name);
          return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid GUID format");
        }
      use_filter = 1;
    }

  if (!name)
    return grub_errno;

  while (1)
    {
      name_size = cap;
      st = r->get_next_variable_name (&name_size, name, &guid);
      if (st == GRUB_EFI_NOT_FOUND)
        break;

      if (st == GRUB_EFI_BUFFER_TOO_SMALL)
        {
          grub_efi_char16_t *new_name;
          cap = name_size;
          new_name = grub_realloc (name, cap);
          if (!new_name)
            {
              grub_free (name);
              return grub_errno;
            }
          name = new_name;
          continue;
        }

      if (st != GRUB_EFI_SUCCESS)
        {
          grub_free (name);
          return grub_error (GRUB_ERR_IO,
                             "get_next_variable_name failed: 0x%lx",
                             (unsigned long) st);
        }

      if (!use_filter || guid_equal (&guid, &filter_guid))
        {
          char *name8 = utf16_name_to_utf8 (name);
          if (!name8)
            {
              grub_free (name);
              return grub_errno;
            }
          grub_printf ("%s %pG\n", name8, &guid);
          grub_free (name8);
        }
    }

  grub_free (name);
  return GRUB_ERR_NONE;
}

static grub_err_t
handle_get (int argc, char **argv)
{
  grub_guid_t guid;
  grub_size_t sz = 0;
  void *data = NULL;
  grub_efi_uint32_t attrs = 0;
  grub_efi_status_t st;

  if (argc != 3)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "usage: efivar get NAME GUID");

  if (!parse_guid (argv[2], &guid))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid GUID format");

  st = grub_efi_get_variable_with_attributes (argv[1], &guid, &sz, &data, &attrs);
  if (st == GRUB_EFI_NOT_FOUND)
    return grub_error (GRUB_ERR_FILE_NOT_FOUND, "variable not found");
  if (st != GRUB_EFI_SUCCESS)
    return grub_error (GRUB_ERR_IO, "get variable failed: 0x%lx",
                       (unsigned long) st);

  grub_printf ("name=%s guid=%pG attrs=0x%08x size=%llu\n",
               argv[1], &guid, attrs, (unsigned long long) sz);
  if (data && sz)
    print_hex_data ((const grub_uint8_t *) data, sz);
  grub_free (data);
  return GRUB_ERR_NONE;
}

static grub_err_t
handle_getstr (int argc, char **argv)
{
  grub_guid_t guid;
  grub_size_t sz = 0;
  void *data = NULL;
  grub_efi_status_t st;
  char *str;

  if (argc != 3)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "usage: efivar getstr NAME GUID");

  if (!parse_guid (argv[2], &guid))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid GUID format");

  st = grub_efi_get_variable (argv[1], &guid, &sz, &data);
  if (st == GRUB_EFI_NOT_FOUND)
    return grub_error (GRUB_ERR_FILE_NOT_FOUND, "variable not found");
  if (st != GRUB_EFI_SUCCESS)
    return grub_error (GRUB_ERR_IO, "get variable failed: 0x%lx",
                       (unsigned long) st);

  str = efi_data_to_utf8 (data, sz);
  if (!str)
    {
      grub_free (data);
      return grub_error (GRUB_ERR_BAD_ARGUMENT,
                         "variable is not valid UTF-16 data");
    }

  grub_printf ("%s\n", str);
  grub_free (str);
  grub_free (data);
  return GRUB_ERR_NONE;
}

static grub_err_t
handle_del (int argc, char **argv)
{
  grub_guid_t guid;

  if (argc != 3)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "usage: efivar del NAME GUID");

  if (!parse_guid (argv[2], &guid))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid GUID format");

  return grub_efi_set_variable_with_attributes (argv[1], &guid, NULL, 0, 0);
}

static grub_err_t
handle_set_string (int argc, char **argv)
{
  grub_guid_t guid;
  grub_efi_uint32_t attrs = (GRUB_EFI_VARIABLE_NON_VOLATILE
                             | GRUB_EFI_VARIABLE_BOOTSERVICE_ACCESS
                             | GRUB_EFI_VARIABLE_RUNTIME_ACCESS);

  if (argc != 4 && argc != 5)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "usage: efivar set NAME GUID STRING [ATTR_HEX]");

  if (!parse_guid (argv[2], &guid))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid GUID format");

  if (argc == 5)
    attrs = grub_strtoul (argv[4], NULL, 0);

  return grub_efi_set_variable_with_attributes (argv[1], &guid, argv[3],
                                                grub_strlen (argv[3]), attrs);
}

static grub_err_t
handle_set_string_utf16 (int argc, char **argv)
{
  grub_guid_t guid;
  grub_efi_uint32_t attrs = (GRUB_EFI_VARIABLE_NON_VOLATILE
                             | GRUB_EFI_VARIABLE_BOOTSERVICE_ACCESS
                             | GRUB_EFI_VARIABLE_RUNTIME_ACCESS);
  grub_uint16_t *str16 = NULL;
  grub_ssize_t len16;
  grub_err_t err;

  if (argc != 4 && argc != 5)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "usage: efivar setstr NAME GUID STRING [ATTR_HEX]");

  if (!parse_guid (argv[2], &guid))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid GUID format");

  if (argc == 5)
    attrs = grub_strtoul (argv[4], NULL, 0);

  len16 = grub_utf8_to_utf16_alloc (argv[3], &str16, NULL);
  if (len16 < 0 || !str16)
    return grub_errno ? grub_errno : grub_error (GRUB_ERR_BAD_ARGUMENT, "UTF-16 conversion failed");

  /* Write with trailing UTF-16 NUL. */
  err = grub_efi_set_variable_with_attributes (argv[1], &guid, str16,
                                               (len16 + 1) * sizeof (str16[0]), attrs);
  grub_free (str16);
  return err;
}

static grub_err_t
handle_set_hex (int argc, char **argv)
{
  grub_guid_t guid;
  grub_efi_uint32_t attrs = (GRUB_EFI_VARIABLE_NON_VOLATILE
                             | GRUB_EFI_VARIABLE_BOOTSERVICE_ACCESS
                             | GRUB_EFI_VARIABLE_RUNTIME_ACCESS);
  grub_size_t hexlen, len, i;
  grub_uint8_t *buf;

  if (argc != 4 && argc != 5)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "usage: efivar sethex NAME GUID HEXDATA [ATTR_HEX]");

  if (!parse_guid (argv[2], &guid))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid GUID format");

  if (argc == 5)
    attrs = grub_strtoul (argv[4], NULL, 0);

  hexlen = grub_strlen (argv[3]);
  if ((hexlen & 1) != 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "HEXDATA length must be even");

  len = hexlen / 2;
  buf = grub_malloc (len ? len : 1);
  if (!buf)
    return grub_errno;

  for (i = 0; i < len; i++)
    {
      int hi = hex_nibble (argv[3][2 * i]);
      int lo = hex_nibble (argv[3][2 * i + 1]);
      if (hi < 0 || lo < 0)
        {
          grub_free (buf);
          return grub_error (GRUB_ERR_BAD_ARGUMENT, "HEXDATA contains non-hex character");
        }
      buf[i] = (hi << 4) | lo;
    }

  grub_errno = grub_efi_set_variable_with_attributes (argv[1], &guid, buf, len, attrs);
  grub_free (buf);
  return grub_errno;
}

static grub_err_t
grub_cmd_getenv (grub_extcmd_context_t ctxt, int argc, char **argv)
{
  grub_guid_t guid;
  enum efivar_value_type type = EFIVAR_TYPE_HEX;
  grub_size_t sz = 0;
  void *data = NULL;
  grub_efi_status_t st;
  grub_err_t err;
  char *out = NULL;

  if (argc != 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "usage: getenv [--guid GUID] [--type TYPE] EFI_ENV VARIABLE");

  err = get_guid_from_args (ctxt, &guid);
  if (err != GRUB_ERR_NONE)
    return err;

  if (ctxt->state[1].set && !parse_type (ctxt->state[1].arg, &type))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid type");

  st = grub_efi_get_variable (argv[0], &guid, &sz, &data);
  if (st == GRUB_EFI_NOT_FOUND)
    return grub_error (GRUB_ERR_FILE_NOT_FOUND, "variable not found");
  if (st != GRUB_EFI_SUCCESS)
    return grub_error (GRUB_ERR_IO, "get variable failed: 0x%lx",
		       (unsigned long) st);

  switch (type)
    {
    case EFIVAR_TYPE_STRING:
      out = grub_malloc (sz + 1);
      if (!out)
	{
	  grub_free (data);
	  return grub_errno;
	}
      if (sz)
	grub_memcpy (out, data, sz);
      out[sz] = '\0';
      break;
    case EFIVAR_TYPE_WSTRING:
      out = efi_data_to_utf8 (data, sz);
      if (!out)
	{
	  grub_free (data);
	  return grub_error (GRUB_ERR_BAD_ARGUMENT,
			     "variable is not valid UTF-16 data");
	}
      break;
    case EFIVAR_TYPE_UINT8:
      if (sz < 1)
	{
	  grub_free (data);
	  return grub_error (GRUB_ERR_BAD_ARGUMENT, "variable data is empty");
	}
      out = grub_xasprintf ("%u", ((const grub_uint8_t *) data)[0]);
      if (!out)
	{
	  grub_free (data);
	  return grub_errno;
	}
      break;
    case EFIVAR_TYPE_HEX:
    default:
      out = hex_data_to_string ((const grub_uint8_t *) data, sz);
      if (!out)
	{
	  grub_free (data);
	  return grub_errno;
	}
      break;
    }

  grub_free (data);
  err = grub_env_set (argv[1], out);
  grub_free (out);
  return err;
}

static grub_err_t
grub_cmd_setenv_efi (grub_extcmd_context_t ctxt, int argc, char **argv)
{
  grub_guid_t guid;
  enum efivar_value_type type = EFIVAR_TYPE_HEX;
  grub_efi_uint32_t attrs = (GRUB_EFI_VARIABLE_BOOTSERVICE_ACCESS
			     | GRUB_EFI_VARIABLE_RUNTIME_ACCESS);
  grub_err_t err;

  if (argc != 2)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "usage: setenv [--guid GUID] [--type TYPE] [--nv] EFI_ENV VALUE");

  err = get_guid_from_args (ctxt, &guid);
  if (err != GRUB_ERR_NONE)
    return err;

  if (ctxt->state[1].set && !parse_type (ctxt->state[1].arg, &type))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid type");

  if (ctxt->state[2].set)
    attrs |= GRUB_EFI_VARIABLE_NON_VOLATILE;

  switch (type)
    {
    case EFIVAR_TYPE_STRING:
      return grub_efi_set_variable_with_attributes (argv[0], &guid, argv[1],
						    grub_strlen (argv[1]), attrs);
    case EFIVAR_TYPE_WSTRING:
      {
	grub_uint16_t *str16 = NULL;
	grub_ssize_t len16 = grub_utf8_to_utf16_alloc (argv[1], &str16, NULL);
	if (len16 < 0 || !str16)
	  return grub_errno ? grub_errno
	    : grub_error (GRUB_ERR_BAD_ARGUMENT, "UTF-16 conversion failed");
	err = grub_efi_set_variable_with_attributes (argv[0], &guid, str16,
						     (len16 + 1) * sizeof (str16[0]),
						     attrs);
	grub_free (str16);
	return err;
      }
    case EFIVAR_TYPE_UINT8:
      {
	const char *end;
	unsigned long v;
	grub_uint8_t b;
	grub_errno = GRUB_ERR_NONE;
	v = grub_strtoul (argv[1], &end, 0);
	if (grub_errno != GRUB_ERR_NONE || (end && *end) || v > 0xff)
	  return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid uint8 value");
	b = (grub_uint8_t) v;
	return grub_efi_set_variable_with_attributes (argv[0], &guid, &b, 1, attrs);
      }
    case EFIVAR_TYPE_HEX:
    default:
      {
	grub_size_t hexlen = grub_strlen (argv[1]);
	grub_size_t len, i;
	grub_uint8_t *buf;

	if ((hexlen & 1) != 0)
	  return grub_error (GRUB_ERR_BAD_ARGUMENT,
			     "HEXDATA length must be even");

	len = hexlen / 2;
	buf = grub_malloc (len ? len : 1);
	if (!buf)
	  return grub_errno;

	for (i = 0; i < len; i++)
	  {
	    int hi = hex_nibble (argv[1][2 * i]);
	    int lo = hex_nibble (argv[1][2 * i + 1]);
	    if (hi < 0 || lo < 0)
	      {
		grub_free (buf);
		return grub_error (GRUB_ERR_BAD_ARGUMENT,
				   "HEXDATA contains non-hex character");
	      }
	    buf[i] = (hi << 4) | lo;
	  }

	grub_errno = grub_efi_set_variable_with_attributes (argv[0], &guid,
							    buf, len, attrs);
	grub_free (buf);
	return grub_errno;
      }
    }
}

static grub_err_t
grub_cmd_lsefienv (grub_extcmd_context_t ctxt, int argc,
		   char **argv __attribute__ ((unused)))
{
  char *list_argv[2];

  if (argc != 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "usage: lsefienv [--guid GUID]");

  list_argv[0] = (char *) "list";
  if (ctxt->state[0].set)
    {
      list_argv[1] = ctxt->state[0].arg;
      return handle_list (2, list_argv);
    }

  return handle_list (1, list_argv);
}

static grub_err_t
grub_cmd_efivar (grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "usage: efivar <get|set|sethex|del> ...");

  if (grub_strcmp (argv[0], "get") == 0)
    return handle_get (argc, argv);
  if (grub_strcmp (argv[0], "getstr") == 0)
    return handle_getstr (argc, argv);
  if (grub_strcmp (argv[0], "list") == 0)
    return handle_list (argc, argv);
  if (grub_strcmp (argv[0], "set") == 0)
    return handle_set_string (argc, argv);
  if (grub_strcmp (argv[0], "setstr") == 0)
    return handle_set_string_utf16 (argc, argv);
  if (grub_strcmp (argv[0], "sethex") == 0)
    return handle_set_hex (argc, argv);
  if (grub_strcmp (argv[0], "del") == 0)
    return handle_del (argc, argv);

  return grub_error (GRUB_ERR_BAD_ARGUMENT,
                     "unknown subcommand: %s", argv[0]);
}

GRUB_MOD_INIT (efivar)
{
  cmd_efivar = grub_register_command ("efivar", grub_cmd_efivar, 0,
                                      N_("efivar <get|getstr|list|set|setstr|sethex|del> ..."));
  cmd_getenv = grub_register_extcmd ("getenv", grub_cmd_getenv, 0,
				     N_("EFI_ENV VARIABLE"),
				     N_("Read EFI variable and store to a GRUB variable."),
				     efienv_options);
  cmd_setenv = grub_register_extcmd ("setenv", grub_cmd_setenv_efi, 0,
				     N_("EFI_ENV VALUE"),
				     N_("Write EFI variable."),
				     efienv_set_options);
  cmd_lsefienv = grub_register_extcmd ("lsefienv", grub_cmd_lsefienv, 0,
				       N_(""),
				       N_("List EFI variables."),
				       efienv_list_options);
}

GRUB_MOD_FINI (efivar)
{
  if (cmd_lsefienv)
    grub_unregister_extcmd (cmd_lsefienv);
  if (cmd_setenv)
    grub_unregister_extcmd (cmd_setenv);
  if (cmd_getenv)
    grub_unregister_extcmd (cmd_getenv);
  if (cmd_efivar)
    grub_unregister_command (cmd_efivar);
}
