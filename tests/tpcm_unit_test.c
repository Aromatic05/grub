/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2026  Free Software Foundation, Inc.
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

#include <setjmp.h>
#include <stdlib.h>

#include <grub/efi/tpcm.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/test.h>
#include <grub/verify.h>

#define TEST_HANDLE ((grub_efi_handle_t) (grub_addr_t) 0x1234)

extern struct grub_file_verifier grub_tpcm_verifier;
struct grub_file_verifier *grub_file_verifiers;

struct tpcm_test_state
{
  int return_handles;
  grub_efi_uintn_t num_handles;
  int open_succeeds;
  grub_efi_boolean_t verify_enabled;
  grub_efi_status_t verify_status;
  grub_uint32_t measure_result;
  grub_uint32_t control_result;

  unsigned int locate_calls;
  unsigned int open_calls;
  unsigned int enabled_calls;
  unsigned int verify_calls;
  grub_efi_locate_search_type_t search_type;
  grub_guid_t locate_guid;
  grub_efi_uint32_t open_attributes;
  grub_efi_handle_t opened_handle;
  grub_guid_t open_guid;

  grub_uint32_t stage;
  grub_uint64_t image_info;
  grub_uint32_t image_info_size;
  grub_uint32_t range_count;
  struct addr_range range;
  char description[TPCM_MAX_BUF_SIZE];
};

static struct tpcm_test_state state;
static grub_uint32_t expected_stage = STAGE_START;
static jmp_buf fatal_env;
static int expect_fatal;
static unsigned int fatal_calls;

void __attribute__ ((noreturn))
__wrap_grub_fatal (const char *fmt, ...);

static grub_efi_status_t __grub_efi_api
mock_verify_raw (struct c2p_protocol *this __attribute__ ((unused)),
		 grub_uint32_t stage, grub_uint64_t image_info,
		 grub_uint32_t image_info_size, grub_uint32_t num_addr_range,
		 struct addr_range ranges[], grub_uint32_t *measure_result,
		 grub_uint32_t *control_result)
{
  grub_size_t copy_size;

  state.verify_calls++;
  state.stage = stage;
  state.image_info = image_info;
  state.image_info_size = image_info_size;
  state.range_count = num_addr_range;
  if (num_addr_range > 0)
    state.range = ranges[0];

  copy_size = image_info_size;
  if (copy_size >= sizeof (state.description))
    copy_size = sizeof (state.description) - 1;
  grub_memcpy (state.description, (void *) (grub_addr_t) image_info,
	       copy_size);
  state.description[copy_size] = '\0';

  *measure_result = state.measure_result;
  *control_result = state.control_result;
  return state.verify_status;
}

static grub_efi_boolean_t __grub_efi_api
mock_verify_is_enabled (struct c2p_protocol *this __attribute__ ((unused)))
{
  state.enabled_calls++;
  return state.verify_enabled;
}

static struct c2p_protocol mock_protocol =
{
  .verify_raw = mock_verify_raw,
  .verify_is_enabled = mock_verify_is_enabled
};

static void
reset_state (void)
{
  grub_memset (&state, 0, sizeof (state));
  state.return_handles = 1;
  state.num_handles = 1;
  state.open_succeeds = 1;
  state.verify_enabled = true;
  state.verify_status = GRUB_EFI_SUCCESS;
}

grub_efi_handle_t *
grub_efi_locate_handle (grub_efi_locate_search_type_t search_type,
			grub_guid_t *protocol,
			void *search_key __attribute__ ((unused)),
			grub_efi_uintn_t *num_handles)
{
  grub_efi_handle_t *handles;
  grub_efi_uintn_t allocation_count;
  grub_efi_uintn_t i;

  state.locate_calls++;
  state.search_type = search_type;
  state.locate_guid = *protocol;
  *num_handles = state.num_handles;
  if (!state.return_handles)
    return NULL;

  allocation_count = state.num_handles ? state.num_handles : 1;
  handles = grub_malloc (allocation_count * sizeof (*handles));
  if (handles == NULL)
    return NULL;

  for (i = 0; i < allocation_count; i++)
    handles[i] = TEST_HANDLE;

  return handles;
}

void *
grub_efi_open_protocol (grub_efi_handle_t handle, grub_guid_t *protocol,
			grub_efi_uint32_t attributes)
{
  state.open_calls++;
  state.opened_handle = handle;
  state.open_guid = *protocol;
  state.open_attributes = attributes;

  return state.open_succeeds ? &mock_protocol : NULL;
}

void __attribute__ ((noreturn))
__wrap_grub_fatal (const char *fmt __attribute__ ((unused)), ...)
{
  fatal_calls++;
  if (expect_fatal)
    longjmp (fatal_env, 1);
  abort ();
}

static int
guid_equal (const grub_guid_t *left, const grub_guid_t *right)
{
  return grub_memcmp (left, right, sizeof (*left)) == 0;
}

static void
assert_successful_measurement (grub_addr_t address, grub_size_t size,
			       const char *description)
{
  grub_test_assert (state.verify_calls == 1,
		    "verify_raw was called %u times", state.verify_calls);
  grub_test_assert (state.stage == expected_stage,
		    "stage %u expected, got %u", expected_stage, state.stage);
  grub_test_assert (state.image_info_size == grub_strlen (description) + 1,
		    "unexpected description size: %u", state.image_info_size);
  grub_test_assert (grub_strcmp (state.description, description) == 0,
		    "description \"%s\" expected, got \"%s\"",
		    description, state.description);
  grub_test_assert (state.range_count == 1,
		    "unexpected address range count: %u", state.range_count);
  grub_test_assert (state.range.start == address,
		    "range start 0x%lx expected, got 0x%llx",
		    (unsigned long) address,
		    (unsigned long long) state.range.start);
  grub_test_assert (state.range.length == size,
		    "range length %lu expected, got %llu",
		    (unsigned long) size,
		    (unsigned long long) state.range.length);
  expected_stage++;
}

static void
test_parse_context (void)
{
  enum grub_file_type type;
  char *description;
  char valid[] = "7|kernel";
  char no_separator[] = "7kernel";
  char empty_type[] = "|kernel";
  char empty_description[] = "7|";
  char long_type[TPCM_MAX_BUF_SIZE + 2];

  grub_test_assert (grub_tpcm_parse_context (valid, &type, &description)
		    == GRUB_ERR_NONE,
		    "valid context was rejected");
  grub_test_assert (type == 7, "unexpected parsed type: %d", type);
  grub_test_assert (grub_strcmp (description, "kernel") == 0,
		    "unexpected description: %s", description);

  grub_test_assert (grub_tpcm_parse_context (NULL, &type, &description)
		    == GRUB_ERR_BUG,
		    "NULL context was accepted");
  grub_test_assert (grub_tpcm_parse_context (valid, NULL, &description)
		    == GRUB_ERR_BAD_ARGUMENT,
		    "NULL type output was accepted");
  grub_test_assert (grub_tpcm_parse_context (valid, &type, NULL)
		    == GRUB_ERR_BAD_ARGUMENT,
		    "NULL description output was accepted");
  grub_test_assert (grub_tpcm_parse_context (no_separator, &type,
					     &description)
		    == GRUB_ERR_BAD_ARGUMENT,
		    "context without separator was accepted");
  grub_test_assert (grub_tpcm_parse_context (empty_type, &type, &description)
		    == GRUB_ERR_BAD_ARGUMENT,
		    "context with an empty type was accepted");

  grub_memset (long_type, '1', TPCM_MAX_BUF_SIZE);
  long_type[TPCM_MAX_BUF_SIZE] = '|';
  long_type[TPCM_MAX_BUF_SIZE + 1] = '\0';
  grub_test_assert (grub_tpcm_parse_context (long_type, &type, &description)
		    == GRUB_ERR_BAD_ARGUMENT,
		    "oversized type was accepted");

  grub_test_assert (grub_tpcm_parse_context (empty_description, &type,
					     &description)
		    == GRUB_ERR_NONE,
		    "empty description was rejected");
  grub_test_assert (*description == '\0',
		    "empty description was parsed incorrectly");
}

static void
test_protocol_states (void)
{
  char context[] = "1|protocol-state";
  char buffer[] = "data";
  grub_err_t err;

  reset_state ();
  state.return_handles = 0;
  err = grub_tpcm_measure_memory (context, (grub_addr_t) buffer,
				 sizeof (buffer));
  grub_test_assert (err == GRUB_ERR_NONE,
		    "missing protocol returned error %d", err);
  grub_test_assert (state.locate_calls == 1 && state.open_calls == 0,
		    "unexpected calls for missing protocol");

  reset_state ();
  state.num_handles = 0;
  err = grub_tpcm_measure_memory (context, (grub_addr_t) buffer,
				 sizeof (buffer));
  grub_test_assert (err == GRUB_ERR_NONE,
		    "empty handle list returned error %d", err);
  grub_test_assert (state.locate_calls == 1 && state.open_calls == 0,
		    "unexpected calls for empty handle list");

  reset_state ();
  state.open_succeeds = 0;
  err = grub_tpcm_measure_memory (context, (grub_addr_t) buffer,
				 sizeof (buffer));
  grub_test_assert (err == GRUB_ERR_BAD_OS,
		    "open failure returned error %d", err);
  grub_test_assert (state.open_calls == 1 && state.enabled_calls == 0,
		    "unexpected calls after open failure");

  reset_state ();
  state.verify_enabled = false;
  err = grub_tpcm_measure_memory (context, (grub_addr_t) buffer,
				 sizeof (buffer));
  grub_test_assert (err == GRUB_ERR_NONE,
		    "disabled verification returned error %d", err);
  grub_test_assert (state.enabled_calls == 1 && state.verify_calls == 0,
		    "verify_raw called while verification was disabled");

  reset_state ();
  state.measure_result = 0xffffffff;
  state.control_result = MEASURE_ACTION_MASK << 1;
  err = grub_tpcm_measure_memory (context, (grub_addr_t) buffer,
				 sizeof (buffer));
  grub_test_assert (err == GRUB_ERR_NONE,
		    "non-blocking firmware results returned error %d", err);
  assert_successful_measurement ((grub_addr_t) buffer, sizeof (buffer),
				 "protocol-state");
}

static void
test_file_verifier (void)
{
  struct grub_file file;
  enum grub_verify_flags flags = GRUB_VERIFY_FLAGS_DEFER_AUTH;
  enum grub_file_type parsed_type;
  enum grub_file_type file_type;
  char *context;
  char *description;
  char file_name[] = "kernel.img";
  char buffer[] = "kernel-data";
  grub_guid_t expected_guid = GRUB_EFI_TPCM_PROTOCOL_GUID;
  grub_err_t err;

  grub_memset (&file, 0, sizeof (file));
  file.name = file_name;
  file_type = GRUB_FILE_TYPE_LINUX_KERNEL | GRUB_FILE_TYPE_SKIP_SIGNATURE;

  err = grub_tpcm_verifier.init (&file, file_type, (void **) &context, &flags);
  grub_test_assert (err == GRUB_ERR_NONE, "file init returned error %d", err);
  grub_test_assert ((flags & GRUB_VERIFY_FLAGS_DEFER_AUTH) != 0,
		    "file init discarded existing flags");
  grub_test_assert ((flags & GRUB_VERIFY_FLAGS_SINGLE_CHUNK) != 0,
		    "file init did not request a single chunk");
  grub_test_assert (grub_tpcm_parse_context (context, &parsed_type,
					     &description)
		    == GRUB_ERR_NONE,
		    "file init generated an invalid context");
  grub_test_assert (parsed_type == GRUB_FILE_TYPE_LINUX_KERNEL,
		    "file type was not masked: %d", parsed_type);
  grub_test_assert (grub_strcmp (description, file.name) == 0,
		    "file name missing from context: %s", description);

  reset_state ();
  err = grub_tpcm_verifier.write (context, buffer, sizeof (buffer));
  grub_test_assert (err == GRUB_ERR_NONE, "file write returned error %d", err);
  grub_test_assert (state.search_type == GRUB_EFI_BY_PROTOCOL,
		    "unexpected EFI search type: %d", state.search_type);
  grub_test_assert (guid_equal (&state.locate_guid, &expected_guid),
		    "locate used the wrong protocol GUID");
  grub_test_assert (state.opened_handle == TEST_HANDLE,
		    "open used the wrong handle");
  grub_test_assert (guid_equal (&state.open_guid, &expected_guid),
		    "open used the wrong protocol GUID");
  grub_test_assert (state.open_attributes
		    == GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL,
		    "unexpected open attributes: %u", state.open_attributes);
  assert_successful_measurement ((grub_addr_t) buffer, sizeof (buffer),
				 file.name);

  grub_test_assert (grub_tpcm_verifier.close != NULL,
		    "file verifier has no context cleanup callback");
  if (grub_tpcm_verifier.close != NULL)
    grub_tpcm_verifier.close (context);
}

static void
test_file_context_lifetime (void)
{
  struct grub_file first_file;
  struct grub_file second_file;
  enum grub_verify_flags first_flags = GRUB_VERIFY_FLAGS_NONE;
  enum grub_verify_flags second_flags = GRUB_VERIFY_FLAGS_NONE;
  enum grub_file_type parsed_type;
  char *first_context;
  char *second_context;
  char *description;
  char first_name[] = "first-kernel.img";
  char long_name[TPCM_MAX_BUF_SIZE + 32];
  grub_err_t err;

  grub_memset (&first_file, 0, sizeof (first_file));
  grub_memset (&second_file, 0, sizeof (second_file));
  grub_memset (long_name, 'a', sizeof (long_name) - 1);
  long_name[sizeof (long_name) - 1] = '\0';
  first_file.name = first_name;
  second_file.name = long_name;

  err = grub_tpcm_verifier.init (&first_file, GRUB_FILE_TYPE_LINUX_KERNEL,
				 (void **) &first_context, &first_flags);
  grub_test_assert (err == GRUB_ERR_NONE,
		    "first file init returned error %d", err);

  err = grub_tpcm_verifier.init (&second_file, GRUB_FILE_TYPE_LINUX_KERNEL,
				 (void **) &second_context, &second_flags);
  grub_test_assert (err == GRUB_ERR_NONE,
		    "second file init returned error %d", err);
  grub_test_assert (first_context != second_context,
		    "file verifier contexts share the same buffer");

  grub_test_assert (grub_tpcm_parse_context (first_context, &parsed_type,
					     &description) == GRUB_ERR_NONE,
		    "first file context is invalid");
  grub_test_assert (grub_strcmp (description, first_name) == 0,
		    "first file context was overwritten: %s", description);

  grub_test_assert (grub_tpcm_parse_context (second_context, &parsed_type,
					     &description) == GRUB_ERR_NONE,
		    "second file context is invalid");
  grub_test_assert (grub_strcmp (description, long_name) == 0,
		    "long file name was truncated");

  grub_test_assert (grub_tpcm_verifier.close != NULL,
		    "file verifier has no context cleanup callback");
  if (grub_tpcm_verifier.close != NULL)
    {
      grub_tpcm_verifier.close (first_context);
      grub_tpcm_verifier.close (second_context);
    }
}

static void
test_one_string (char *string, enum grub_verify_string_type type,
		 const char *expected_description)
{
  grub_err_t err;

  reset_state ();
  err = grub_tpcm_verifier.verify_string (string, type);
  grub_test_assert (err == GRUB_ERR_NONE,
		    "string verifier returned error %d", err);
  assert_successful_measurement ((grub_addr_t) string, grub_strlen (string),
				 expected_description);
}

static void
test_string_verifier (void)
{
  char kernel[] = "root=/dev/vda";
  char module[] = "module-option";
  char command[] = "set root=hd0";
  grub_err_t err;

  test_one_string (kernel, GRUB_VERIFY_KERNEL_CMDLINE,
		   "kernel_cmdline: root=/dev/vda");
  test_one_string (module, GRUB_VERIFY_MODULE_CMDLINE,
		   "module_cmdline: module-option");
  test_one_string (command, GRUB_VERIFY_COMMAND,
		   "grub_cmd: set root=hd0");

  reset_state ();
  err = grub_tpcm_verifier.verify_string (command,
					  (enum grub_verify_string_type) 99);
  grub_test_assert (err == GRUB_ERR_BAD_ARGUMENT,
		    "unknown string type returned error %d", err);
  grub_test_assert (state.locate_calls == 0 && state.verify_calls == 0,
		    "unknown string type contacted firmware");
}

static void
expect_measurement_fatal (grub_efi_status_t verify_status,
			  grub_uint32_t control_result,
			  const char *failure_message)
{
  char context[] = "1|fatal";
  char buffer[] = "data";

  reset_state ();
  state.verify_status = verify_status;
  state.control_result = control_result;
  fatal_calls = 0;
  expect_fatal = 1;

  if (setjmp (fatal_env) == 0)
    {
      grub_tpcm_measure_memory (context, (grub_addr_t) buffer,
				sizeof (buffer));
      grub_test_assert (0, "%s", failure_message);
    }

  expect_fatal = 0;
  grub_test_assert (fatal_calls == 1,
		    "fatal path was entered %u times", fatal_calls);
}

static void
test_fatal_policy (void)
{
  expect_measurement_fatal (GRUB_EFI_DEVICE_ERROR, 0,
			    "EFI verification error did not call grub_fatal");
  expect_measurement_fatal (GRUB_EFI_SUCCESS, MEASURE_ACTION_MASK,
			    "blocking control result did not call grub_fatal");
}

static void
test_stage_exhaustion (void)
{
  char context[] = "1|stage";
  char buffer[] = "data";
  grub_err_t err;

  while (expected_stage <= STAGE_END)
    {
      reset_state ();
      err = grub_tpcm_measure_memory (context, (grub_addr_t) buffer,
				     sizeof (buffer));
      grub_test_assert (err == GRUB_ERR_NONE,
			"stage %u returned error %d", expected_stage, err);
      assert_successful_measurement ((grub_addr_t) buffer, sizeof (buffer),
				     "stage");
    }

  reset_state ();
  err = grub_tpcm_measure_memory (context, (grub_addr_t) buffer,
				 sizeof (buffer));
  grub_test_assert (err == GRUB_ERR_BAD_OS,
		    "exhausted stage range returned error %d", err);
  grub_test_assert (state.verify_calls == 0,
		    "verify_raw was called after stage exhaustion");
}

static void
tpcm_unit_test (void)
{
  test_parse_context ();
  test_protocol_states ();
  test_file_verifier ();
  test_file_context_lifetime ();
  test_string_verifier ();
  test_fatal_policy ();
  test_stage_exhaustion ();
}

GRUB_UNIT_TEST ("tpcm_unit_test", tpcm_unit_test);
