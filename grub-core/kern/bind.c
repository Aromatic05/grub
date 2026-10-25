/* Language-neutral native binding registry. GPL-3.0-or-later. */
#include <grub/bind.h>
#include <grub/mm.h>
#include <grub/misc.h>

struct bind_entry
{
  struct bind_entry *next;
  const struct grub_bind_export *descriptor;
  grub_dl_t owner;
  grub_bind_id_t id;
};

struct grub_bind_handle
{
  grub_dl_t owner;
  char *type;
  void *data;
  void (*destroy) (void *);
};

static struct bind_entry *entries;
static struct grub_bind_runtime *runtimes;
static grub_bind_id_t next_id = 1;

static void
notify_runtimes (void)
{
  struct grub_bind_runtime *runtime;
  for (runtime = runtimes; runtime; runtime = runtime->next)
    if (runtime->changed)
      runtime->changed (runtime->context);
}

grub_err_t
grub_bind_register (grub_dl_t owner, const struct grub_bind_export *exports,
                    grub_size_t count)
{
  struct bind_entry *pending = NULL, *entry;
  grub_size_t i;
  if (!exports || !count)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "empty native binding group");
  for (i = 0; i < count; i++)
    {
      const struct grub_bind_export *exported = &exports[i];
      struct bind_entry *e;
      if (!exported->name_space || !*exported->name_space ||
          !exported->name || !*exported->name ||
          !exported->invoke || exported->max_args > GRUB_BIND_MAX_ARGS ||
          exported->min_args > exported->max_args ||
          (exported->max_args && !exported->arg_types))
        {
          grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid native binding descriptor");
          goto fail;
        }
      for (e = entries; e; e = e->next)
        if (!grub_strcmp (e->descriptor->name_space, exported->name_space) &&
            !grub_strcmp (e->descriptor->name, exported->name))
          {
            grub_error (GRUB_ERR_BAD_ARGUMENT, "duplicate native binding %s.%s",
                        exported->name_space, exported->name);
            goto fail;
          }
      for (e = pending; e; e = e->next)
        if (!grub_strcmp (e->descriptor->name_space, exported->name_space) &&
            !grub_strcmp (e->descriptor->name, exported->name))
          {
            grub_error (GRUB_ERR_BAD_ARGUMENT, "duplicate native binding %s.%s",
                        exported->name_space, exported->name);
            goto fail;
          }
      if (!next_id || next_id == ~(grub_bind_id_t) 0)
        {
          grub_error (GRUB_ERR_OUT_OF_RANGE, "native binding IDs exhausted");
          goto fail;
        }
      entry = grub_zalloc (sizeof (*entry));
      if (!entry) goto fail;
      entry->descriptor = exported;
      entry->owner = owner;
      entry->id = next_id++;
      entry->next = pending;
      pending = entry;
    }
  while (pending)
    {
      entry = pending;
      pending = entry->next;
      entry->next = entries;
      entries = entry;
    }
  notify_runtimes ();
  return GRUB_ERR_NONE;

fail:
  while (pending)
    {
      entry = pending;
      pending = entry->next;
      grub_free (entry);
    }
  return grub_errno;
}

void
grub_bind_unregister (grub_dl_t owner)
{
  struct bind_entry **current = &entries;
  int removed = 0;
  while (*current)
    {
      struct bind_entry *entry = *current;
      if (entry->owner != owner)
        {
          current = &entry->next;
          continue;
        }
      *current = entry->next;
      grub_free (entry);
      removed = 1;
    }
  if (removed) notify_runtimes ();
}

grub_bind_id_t
grub_bind_lookup (const char *name_space, const char *name)
{
  struct bind_entry *entry;
  if (!name_space || !name) return 0;
  for (entry = entries; entry; entry = entry->next)
    if (!grub_strcmp (entry->descriptor->name_space, name_space) &&
        !grub_strcmp (entry->descriptor->name, name))
      return entry->id;
  return 0;
}

int
grub_bind_has_namespace (const char *name_space)
{
  struct bind_entry *entry;
  if (!name_space) return 0;
  for (entry = entries; entry; entry = entry->next)
    if (!grub_strcmp (entry->descriptor->name_space, name_space))
      return 1;
  return 0;
}

void
grub_bind_iterate (grub_bind_iter_hook_t hook, void *data)
{
  struct bind_entry *entry;
  if (!hook) return;
  for (entry = entries; entry; entry = entry->next)
    if (hook (entry->descriptor, entry->id, data))
      break;
}

static struct bind_entry *
find_id (grub_bind_id_t id)
{
  struct bind_entry *entry;
  if (!id) return NULL;
  for (entry = entries; entry; entry = entry->next)
    if (entry->id == id) return entry;
  return NULL;
}

const struct grub_bind_export *
grub_bind_describe (grub_bind_id_t id)
{
  struct bind_entry *entry = find_id (id);
  return entry ? entry->descriptor : NULL;
}

grub_err_t
grub_bind_invoke (grub_bind_id_t id, const struct grub_bind_value *args,
                  grub_size_t argc, struct grub_bind_value *results,
                  grub_size_t *nresults)
{
  struct bind_entry *entry = find_id (id);
  const struct grub_bind_export *exported;
  grub_err_t err;
  grub_size_t i;
  if (!entry)
    return grub_error (GRUB_ERR_BAD_MODULE, "native binding is unavailable");
  exported = entry->descriptor;
  if (argc < exported->min_args || argc > exported->max_args ||
      (argc && !args) || !results || !nresults)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid native binding arguments");
  for (i = 0; i < argc; i++)
    if (args[i].type != exported->arg_types[i])
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "native binding argument type mismatch");
  grub_memset (results, 0, GRUB_BIND_MAX_RESULTS * sizeof (*results));
  *nresults = 0;
  grub_dl_ref (entry->owner);
  err = exported->invoke (argc, args, results, nresults);
  grub_dl_unref (entry->owner);
  if (*nresults > GRUB_BIND_MAX_RESULTS)
    {
      *nresults = GRUB_BIND_MAX_RESULTS;
      err = grub_error (GRUB_ERR_OUT_OF_RANGE, "too many native binding results");
    }
  return err;
}

void
grub_bind_value_dispose (struct grub_bind_value *value)
{
  if (!value) return;
  if (value->type == GRUB_BIND_HANDLE && value->value.handle)
    grub_bind_handle_release (value->value.handle);
  else if (value->owned && value->type == GRUB_BIND_RECORD)
    {
      grub_size_t i;
      struct grub_bind_field *fields =
        (struct grub_bind_field *) value->value.record.fields;
      for (i = 0; i < value->value.record.count; i++)
        grub_bind_value_dispose (&fields[i].value);
      grub_free (fields);
    }
  else if (value->owned &&
           (value->type == GRUB_BIND_STRING || value->type == GRUB_BIND_BYTES))
    grub_free ((void *) value->value.bytes.data);
  grub_memset (value, 0, sizeof (*value));
}

void
grub_bind_runtime_attach (struct grub_bind_runtime *runtime)
{
  if (!runtime) return;
  runtime->next = runtimes;
  runtimes = runtime;
}

void
grub_bind_runtime_detach (struct grub_bind_runtime *runtime)
{
  struct grub_bind_runtime **p;
  if (!runtime) return;
  for (p = &runtimes; *p; p = &(*p)->next)
    if (*p == runtime)
      {
        *p = runtime->next;
        runtime->next = NULL;
        return;
      }
}

struct grub_bind_handle *
grub_bind_handle_create (grub_dl_t owner, const char *type,
                         void *data, void (*destroy) (void *))
{
  struct grub_bind_handle *handle;
  if (!type || !*type || !data || !destroy)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid native handle");
      return NULL;
    }
  handle = grub_zalloc (sizeof (*handle));
  if (!handle) return NULL;
  handle->type = grub_strdup (type);
  if (!handle->type)
    {
      grub_free (handle);
      return NULL;
    }
  handle->owner = owner;
  handle->data = data;
  handle->destroy = destroy;
  grub_dl_ref (owner);
  return handle;
}

void *
grub_bind_handle_data (struct grub_bind_handle *handle,
                       const char *expected_type)
{
  if (!handle || !handle->data || !expected_type ||
      grub_strcmp (handle->type, expected_type))
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid or closed native handle");
      return NULL;
    }
  return handle->data;
}

grub_err_t
grub_bind_handle_close (struct grub_bind_handle *handle)
{
  if (!handle || !handle->data)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "native handle already closed");
  handle->destroy (handle->data);
  handle->data = NULL;
  grub_dl_unref (handle->owner);
  handle->owner = NULL;
  handle->destroy = NULL;
  return GRUB_ERR_NONE;
}

void
grub_bind_handle_release (struct grub_bind_handle *handle)
{
  if (!handle) return;
  if (handle->data) grub_bind_handle_close (handle);
  grub_free (handle->type);
  grub_free (handle);
}
