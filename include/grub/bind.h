/* Language-neutral native binding registry. GPL-3.0-or-later. */
#ifndef GRUB_BIND_HEADER
#define GRUB_BIND_HEADER 1

#include <grub/types.h>
#include <grub/err.h>
#include <grub/dl.h>

/* This ABI is internal to a GRUB build, not a cross-version binary ABI. */
typedef grub_uint64_t grub_bind_id_t;

enum grub_bind_type
{
  GRUB_BIND_NIL,
  GRUB_BIND_BOOL,
  GRUB_BIND_I64,
  GRUB_BIND_U64,
  GRUB_BIND_STRING,
  GRUB_BIND_BYTES,
  GRUB_BIND_RECORD,
  GRUB_BIND_HANDLE
};

struct grub_bind_handle;
struct grub_bind_field;

struct grub_bind_value
{
  enum grub_bind_type type;
  /* The receiver must dispose owned strings/bytes/handles after use. */
  int owned;
  union
  {
    int boolean;
    grub_int64_t i64;
    grub_uint64_t u64;
    struct { const void *data; grub_size_t size; } bytes;
    struct { const struct grub_bind_field *fields; grub_size_t count; } record;
    struct grub_bind_handle *handle;
  } value;
};

/* A record carries named, typed fields.  Strings/field names are borrowed
   for the invocation; a returned record can own its field array and values. */
struct grub_bind_field
{
  const char *name;
  struct grub_bind_value value;
};

/* Functions never receive or return a language-runtime object.  Result
   strings may be borrowed (owned = 0) or heap allocated (owned = 1).
   A successfully returned handle transfers ownership to the consumer. */
typedef grub_err_t (*grub_bind_func_t)
  (grub_size_t argc, const struct grub_bind_value *args,
   struct grub_bind_value *results, grub_size_t *nresults);

struct grub_bind_export
{
  const char *name_space;
  const char *name;
  const enum grub_bind_type *arg_types;
  unsigned min_args, max_args;
  grub_bind_func_t invoke;
};

#define GRUB_BIND_MAX_ARGS 16
#define GRUB_BIND_MAX_RESULTS 8
#define GRUB_BIND_MAX_RECORD_FIELDS 8

/* Registration is per native module; unregister before the module's code
   and descriptors are unloaded.  IDs are never reused after unloading. */
grub_err_t EXPORT_FUNC(grub_bind_register)
  (grub_dl_t owner, const struct grub_bind_export *exports, grub_size_t count);
void EXPORT_FUNC(grub_bind_unregister) (grub_dl_t owner);
grub_bind_id_t EXPORT_FUNC(grub_bind_lookup)
  (const char *name_space, const char *name);
int EXPORT_FUNC(grub_bind_has_namespace) (const char *name_space);
typedef int (*grub_bind_iter_hook_t)
  (const struct grub_bind_export *exported, grub_bind_id_t id, void *data);
void EXPORT_FUNC(grub_bind_iterate) (grub_bind_iter_hook_t hook, void *data);
const struct grub_bind_export *EXPORT_FUNC(grub_bind_describe)
  (grub_bind_id_t id);
grub_err_t EXPORT_FUNC(grub_bind_invoke)
  (grub_bind_id_t id, const struct grub_bind_value *args, grub_size_t argc,
   struct grub_bind_value *results, grub_size_t *nresults);
void EXPORT_FUNC(grub_bind_value_dispose) (struct grub_bind_value *value);

/* Runtime observers are notifications only: never invoke a VM during a
   module's registration or finalization.  Runtime code owns this record. */
struct grub_bind_runtime
{
  struct grub_bind_runtime *next;
  void (*changed) (void *context);
  void *context;
};

void EXPORT_FUNC(grub_bind_runtime_attach) (struct grub_bind_runtime *);
void EXPORT_FUNC(grub_bind_runtime_detach) (struct grub_bind_runtime *);

/* Handles keep the owning native module loaded until explicitly closed
   or released by the language runtime.  Closing invalidates the resource
   but not the opaque handle allocation, allowing safe double-close checks. */
struct grub_bind_handle *EXPORT_FUNC(grub_bind_handle_create)
  (grub_dl_t owner, const char *type, void *data, void (*destroy) (void *));
void *EXPORT_FUNC(grub_bind_handle_data)
  (struct grub_bind_handle *, const char *expected_type);
grub_err_t EXPORT_FUNC(grub_bind_handle_close) (struct grub_bind_handle *);
void EXPORT_FUNC(grub_bind_handle_release) (struct grub_bind_handle *);

#endif
