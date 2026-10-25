/*
 * Native HTTP/1.1 client, shared by GRUB network files and script runtimes.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GRUB_HTTP_HEADER
#define GRUB_HTTP_HEADER 1

#include <grub/types.h>
#include <grub/err.h>

struct grub_http_stream;
typedef struct grub_http_stream *grub_http_stream_t;

struct grub_http_header
{
  const char *name;
  const char *value;
};

struct grub_http_request
{
  const char *url;
  const char *method;  /* NULL: GET */
  const struct grub_http_header *headers;
  grub_size_t header_count;
  const void *body;
  grub_size_t body_length;
};

struct grub_http_response
{
  unsigned status;
  char *headers;  /* raw HTTP header block */
  char *body;     /* may contain NUL: always consult body_length */
  grub_size_t body_length;
};

grub_http_stream_t grub_http_stream_open (const struct grub_http_request *,
                                          grub_uint64_t range_start);
grub_ssize_t grub_http_stream_read (grub_http_stream_t, void *, grub_size_t);
void grub_http_stream_close (grub_http_stream_t);
unsigned grub_http_stream_status (grub_http_stream_t);
const char *grub_http_stream_headers (grub_http_stream_t);
int grub_http_stream_length (grub_http_stream_t, grub_uint64_t *);

/* Buffer at most 8 MiB in the convenience request path. Streaming users
   should use stream_open/read or an opaque Bind handle instead. */
grub_err_t grub_http_request (const struct grub_http_request *,
                              struct grub_http_response *);
void grub_http_response_free (struct grub_http_response *);

#endif
