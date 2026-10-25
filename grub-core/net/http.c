/*
 * GRUB HTTP(S) network filesystem and language-neutral native exports.
 *
 * Both (http,host)/path and (https,host)/path use the same streaming
 * HTTP/1.1 client as native and Lua requests. The on-demand VFS adapter
 * never loads the entire remote file into memory.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <grub/http.h>
#include <grub/net.h>
#include <grub/net/netbuff.h>
#include <grub/bind.h>
#include <grub/dl.h>
#include <grub/file.h>
#include <grub/misc.h>
#include <grub/mm.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define HTTP_VFS_BLOCK 4096
#define HTTP_BIND_READ_MAX 65536

struct http_file_state
{
  grub_http_stream_t stream;
  char *url;
  grub_dl_t owner;
};

/* Shared native HTTP streams also pin the module, including C callers
   which do not use a VFS file or Binding-managed handle. */
grub_dl_t grub_http_module_owner;

static int
is_https_file (grub_file_t file)
{
  return !grub_strcmp (file->device->net->protocol->name, "https");
}

static char *
file_url (grub_file_t file, const char *name)
{
  grub_net_t net = file->device->net;
  char *path;
  char *url;
  const char *scheme = is_https_file (file) ? "https" : "http";
  grub_uint16_t port = net->port ? net->port :
    (is_https_file (file) ? 443 : 80);
  path = *name == '/' ? grub_strdup (name) : grub_xasprintf ("/%s", name);
  if (!path) return NULL;
  url = grub_xasprintf ("%s://%s:%u%s", scheme, net->server, port, path);
  grub_free (path);
  return url;
}

static grub_err_t
open_stream_at (grub_file_t file, grub_uint64_t at)
{
  struct http_file_state *state = file->data;
  struct grub_http_request request = { 0 };
  grub_uint64_t length;
  request.url = state->url;
  request.method = "GET";
  state->stream = grub_http_stream_open (&request, at);
  if (!state->stream) return grub_errno;
  if (grub_http_stream_status (state->stream) == 404)
    return grub_error (GRUB_ERR_FILE_NOT_FOUND, "HTTP file does not exist");
  if (grub_http_stream_status (state->stream) != (at ? 206 : 200))
    return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                       "HTTP file request failed, status %u",
                       grub_http_stream_status (state->stream));
  if (grub_http_stream_length (state->stream, &length))
    {
      if (at > ~(grub_uint64_t) 0 - length)
        return grub_error (GRUB_ERR_OUT_OF_RANGE, "HTTP file length overflow");
      file->size = at + length;
    }
  else if (!at)
    file->size = GRUB_FILE_SIZE_UNKNOWN;
  return GRUB_ERR_NONE;
}

static grub_err_t
http_vfs_open (grub_file_t file, const char *name)
{
  struct http_file_state *state = grub_zalloc (sizeof (*state));
  grub_err_t err;
  if (!state) return grub_errno;
  file->data = state;
  state->owner = grub_http_module_owner;
  state->url = file_url (file, name);
  if (!state->url) goto fail;
  err = open_stream_at (file, 0);
  if (err) goto fail;
  grub_dl_ref (grub_http_module_owner);
  file->not_easily_seekable = 1;
  return GRUB_ERR_NONE;
fail:
  err = grub_errno;
  grub_http_stream_close (state->stream);
  grub_free (state->url);
  grub_free (state);
  file->data = NULL;
  return err;
}

static grub_err_t
http_vfs_close (grub_file_t file)
{
  struct http_file_state *state = file->data;
  if (!state) return GRUB_ERR_NONE;
  grub_http_stream_close (state->stream);
  grub_free (state->url);
  grub_dl_unref (state->owner);
  grub_free (state);
  file->data = NULL;
  return GRUB_ERR_NONE;
}

static grub_err_t
http_vfs_seek (grub_file_t file, grub_off_t offset)
{
  struct http_file_state *state = file->data;
  grub_net_t net = file->device->net;
  grub_err_t err;
  if (!state || (grub_int64_t) offset < 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP seek");
  grub_http_stream_close (state->stream);
  state->stream = NULL;
  while (net->packs.first)
    {
      grub_netbuff_free (net->packs.first->nb);
      grub_net_remove_packet (net->packs.first);
    }
  net->eof = net->broken = net->stall = 0;
  net->offset = offset;
  err = open_stream_at (file, offset);
  if (err) net->broken = 1;
  return err;
}

/* GRUB's network filesystem is packet-oriented.  Feed it one decoded
   HTTP body block only when the caller needs more data: no whole-file
   buffering, and valid TCP/TLS reads cannot be mistaken for HTTP framing. */
static grub_err_t
http_vfs_refill (grub_file_t file)
{
  grub_net_t net = file->device->net;
  struct http_file_state *state = file->data;
  struct grub_net_buff *nb;
  grub_ssize_t read;
  if (!state || !state->stream || net->eof || net->broken ||
      net->packs.count >= 2)
    return GRUB_ERR_NONE;
  nb = grub_netbuff_alloc (HTTP_VFS_BLOCK);
  if (!nb) return grub_errno;
  read = grub_http_stream_read (state->stream, nb->tail, HTTP_VFS_BLOCK);
  if (read < 0)
    {
      net->broken = 1;
      grub_netbuff_free (nb);
      return grub_errno;
    }
  if (!read)
    {
      struct grub_net_packet *pending;
      grub_uint64_t end = net->offset;
      /* Network bufio must learn the exact size at EOF. Otherwise its
         partial-block readahead can seek to zero and append uninitialized
         bytes to a chunked/close-delimited response. */
      for (pending = net->packs.first; pending; pending = pending->next)
        end += pending->nb->tail - pending->nb->data;
      file->size = end;
      net->eof = 1;
      grub_netbuff_free (nb);
      return GRUB_ERR_NONE;
    }
  if (grub_netbuff_put (nb, read))
    { grub_netbuff_free (nb); return grub_errno; }
  if (grub_net_put_packet (&net->packs, nb))
    { grub_netbuff_free (nb); return grub_errno; }
  return GRUB_ERR_NONE;
}

static struct grub_net_app_protocol http_proto =
  { .name = "http", .open = http_vfs_open, .close = http_vfs_close,
    .seek = http_vfs_seek, .packets_pulled = http_vfs_refill };
static struct grub_net_app_protocol https_proto =
  { .name = "https", .open = http_vfs_open, .close = http_vfs_close,
    .seek = http_vfs_seek, .packets_pulled = http_vfs_refill };

static const struct grub_bind_value *
option (const struct grub_bind_value *record, const char *name)
{
  grub_size_t i;
  for (i = 0; i < record->value.record.count; i++)
    {
      const struct grub_bind_field *field = &record->value.record.fields[i];
      if (!grub_strcmp (field->name, name)) return &field->value;
    }
  return NULL;
}

static grub_err_t
get_string_option (const struct grub_bind_value *record, const char *name,
                   const char **out, grub_size_t *size)
{
  const struct grub_bind_value *value = option (record, name);
  if (!value)
    { *out = NULL; if (size) *size = 0; return GRUB_ERR_NONE; }
  if (value->type != GRUB_BIND_STRING &&
      value->type != GRUB_BIND_BYTES)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "HTTP %s must be a string", name);
  if (size) *size = value->value.bytes.size;
  *out = value->value.bytes.data;
  if (grub_strlen (*out) != value->value.bytes.size &&
      grub_strcmp (name, "body"))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "embedded NUL in HTTP %s", name);
  return GRUB_ERR_NONE;
}

static grub_err_t
parse_options (const struct grub_bind_value *record, int secure,
               struct grub_http_request *req,
               struct grub_http_header *headers, grub_size_t capacity)
{
  const struct grub_bind_value *header_record;
  grub_size_t i;
  grub_size_t len;
  const char *body;
  grub_memset (req, 0, sizeof (*req));
  if (get_string_option (record, "url", &req->url, NULL) || !req->url)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "HTTP URL is required");
  if (grub_strncmp (req->url, secure ? "https://" : "http://",
                    secure ? 8 : 7))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "HTTP URL scheme mismatch");
  if (get_string_option (record, "method", &req->method, NULL) ||
      get_string_option (record, "body", &body, &len))
    return grub_errno;
  req->body = body;
  req->body_length = len;
  header_record = option (record, "headers");
  if (!header_record)
    return GRUB_ERR_NONE;
  if (header_record->type != GRUB_BIND_RECORD ||
      header_record->value.record.count > capacity)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP header record");
  for (i = 0; i < header_record->value.record.count; ++i)
    {
      const struct grub_bind_field *field = &header_record->value.record.fields[i];
      if (field->value.type != GRUB_BIND_STRING ||
          grub_strlen (field->value.value.bytes.data) !=
              field->value.value.bytes.size)
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "HTTP header values must be text");
      headers[i].name = field->name;
      headers[i].value = field->value.value.bytes.data;
    }
  req->headers = headers;
  req->header_count = header_record->value.record.count;
  return GRUB_ERR_NONE;
}

static grub_err_t
response_result (const struct grub_http_response *response,
                 struct grub_bind_value *result)
{
  struct grub_bind_field *fields = grub_zalloc (3 * sizeof (*fields));
  if (!fields) return grub_errno;
  fields[0].name = "status";
  fields[0].value.type = GRUB_BIND_U64;
  fields[0].value.value.u64 = response->status;
  fields[1].name = "headers";
  fields[1].value.type = GRUB_BIND_STRING;
  fields[1].value.value.bytes.data = response->headers;
  fields[1].value.value.bytes.size = grub_strlen (response->headers);
  fields[2].name = "body";
  fields[2].value.type = GRUB_BIND_BYTES;
  fields[2].value.value.bytes.data = response->body;
  fields[2].value.value.bytes.size = response->body_length;
  result->type = GRUB_BIND_RECORD;
  result->owned = 1;
  result->value.record.fields = fields;
  result->value.record.count = 3;
  return GRUB_ERR_NONE;
}

static grub_err_t
request_impl (int secure, const struct grub_bind_value *args,
              struct grub_bind_value *results, grub_size_t *count)
{
  struct grub_http_request request;
  struct grub_http_header headers[GRUB_BIND_MAX_RECORD_FIELDS];
  struct grub_http_response response = {0};
  grub_err_t err;
  if (parse_options (&args[0], secure, &request, headers,
                     GRUB_BIND_MAX_RECORD_FIELDS))
    return grub_errno;
  err = grub_http_request (&request, &response);
  if (err) return err;
  err = response_result (&response, &results[0]);
  if (err) { grub_http_response_free (&response); return err; }
  /* Response's strings now belong to the Bind record and Lua adapter. */
  ((struct grub_bind_field *) results[0].value.record.fields)[1].value.owned = 1;
  ((struct grub_bind_field *) results[0].value.record.fields)[2].value.owned = 1;
  *count = 1;
  return GRUB_ERR_NONE;
}

static void
destroy_stream (void *value)
{
  grub_http_stream_close (value);
}

static grub_err_t
open_impl (int secure, const struct grub_bind_value *args,
           struct grub_bind_value *results, grub_size_t *count)
{
  struct grub_http_request request;
  struct grub_http_header headers[GRUB_BIND_MAX_RECORD_FIELDS];
  struct grub_bind_handle *handle;
  grub_http_stream_t stream;
  if (parse_options (&args[0], secure, &request, headers,
                     GRUB_BIND_MAX_RECORD_FIELDS))
    return grub_errno;
  stream = grub_http_stream_open (&request, 0);
  if (!stream) return grub_errno;
  handle = grub_bind_handle_create (grub_http_module_owner, "http.stream",
                                    stream, destroy_stream);
  if (!handle)
    { grub_http_stream_close (stream); return grub_errno; }
  results[0].type = GRUB_BIND_HANDLE;
  results[0].value.handle = handle;
  *count = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_request_http (grub_size_t argc __attribute__ ((unused)),
                   const struct grub_bind_value *args,
                   struct grub_bind_value *out, grub_size_t *count)
{ return request_impl (0, args, out, count); }

static grub_err_t
bind_request_https (grub_size_t argc __attribute__ ((unused)),
                    const struct grub_bind_value *args,
                    struct grub_bind_value *out, grub_size_t *count)
{ return request_impl (1, args, out, count); }

static grub_err_t
bind_open_http (grub_size_t argc __attribute__ ((unused)),
                const struct grub_bind_value *args,
                struct grub_bind_value *out, grub_size_t *count)
{ return open_impl (0, args, out, count); }

static grub_err_t
bind_open_https (grub_size_t argc __attribute__ ((unused)),
                 const struct grub_bind_value *args,
                 struct grub_bind_value *out, grub_size_t *count)
{ return open_impl (1, args, out, count); }

static grub_err_t
bind_read (grub_size_t argc __attribute__ ((unused)),
           const struct grub_bind_value *args,
           struct grub_bind_value *out, grub_size_t *count)
{
  grub_http_stream_t stream = grub_bind_handle_data (args[0].value.handle,
                                                     "http.stream");
  grub_uint64_t length = args[1].value.u64;
  grub_ssize_t read;
  char *buffer;
  if (!stream) return grub_errno;
  if (!length || length > HTTP_BIND_READ_MAX)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "HTTP read size must be 1..65536");
  buffer = grub_malloc (length);
  if (!buffer) return grub_errno;
  read = grub_http_stream_read (stream, buffer, (grub_size_t) length);
  if (read < 0)
    { grub_free (buffer); return grub_errno; }
  out[0].type = GRUB_BIND_BYTES;
  out[0].owned = 1;
  out[0].value.bytes.data = buffer;
  out[0].value.bytes.size = read;
  *count = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_status (grub_size_t argc __attribute__ ((unused)),
             const struct grub_bind_value *args,
             struct grub_bind_value *out, grub_size_t *count)
{
  grub_http_stream_t stream = grub_bind_handle_data (args[0].value.handle,
                                                     "http.stream");
  struct grub_bind_field *fields;
  if (!stream) return grub_errno;
  fields = grub_zalloc (2 * sizeof (*fields));
  if (!fields) return grub_errno;
  fields[0].name = "status";
  fields[0].value.type = GRUB_BIND_U64;
  fields[0].value.value.u64 = grub_http_stream_status (stream);
  fields[1].name = "headers";
  fields[1].value.type = GRUB_BIND_STRING;
  fields[1].value.value.bytes.data = grub_http_stream_headers (stream);
  fields[1].value.value.bytes.size =
    grub_strlen (fields[1].value.value.bytes.data);
  out[0].type = GRUB_BIND_RECORD;
  out[0].owned = 1;
  out[0].value.record.fields = fields;
  out[0].value.record.count = 2;
  *count = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_close (grub_size_t argc __attribute__ ((unused)),
            const struct grub_bind_value *args,
            struct grub_bind_value *out __attribute__ ((unused)),
            grub_size_t *count)
{
  if (!grub_bind_handle_data (args[0].value.handle, "http.stream"))
    return grub_errno;
  *count = 0;
  return grub_bind_handle_close (args[0].value.handle);
}

static const enum grub_bind_type options_type[] = { GRUB_BIND_RECORD };
static const enum grub_bind_type read_types[] =
  { GRUB_BIND_HANDLE, GRUB_BIND_U64 };
static const enum grub_bind_type handle_type[] = { GRUB_BIND_HANDLE };
static const struct grub_bind_export exports[] =
{
  { "http", "request", options_type, 1, 1, bind_request_http },
  { "https", "request", options_type, 1, 1, bind_request_https },
  { "http", "request_stream", options_type, 1, 1, bind_open_http },
  { "https", "request_stream", options_type, 1, 1, bind_open_https },
  { "http", "read", read_types, 2, 2, bind_read },
  { "https", "read", read_types, 2, 2, bind_read },
  { "http", "status", handle_type, 1, 1, bind_status },
  { "https", "status", handle_type, 1, 1, bind_status },
  { "http", "close", handle_type, 1, 1, bind_close },
  { "https", "close", handle_type, 1, 1, bind_close }
};

GRUB_MOD_INIT (http)
{
  grub_http_module_owner = mod;
  grub_net_app_level_register (&http_proto);
  grub_net_app_level_register (&https_proto);
  if (grub_bind_register (mod, exports, sizeof (exports) / sizeof (exports[0])))
    grub_print_error ();
}

GRUB_MOD_FINI (http)
{
  grub_bind_unregister (grub_http_module_owner);
  grub_net_app_level_unregister (&https_proto);
  grub_net_app_level_unregister (&http_proto);
  grub_http_module_owner = NULL;
}
