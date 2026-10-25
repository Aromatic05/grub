/*
 * GRUB HTTP/1.1 core: streaming response framing over raw TCP or verified TLS.
 * No language-runtime dependency and no full-body buffering for file access.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <grub/http.h>
#include <grub/tls.h>
#include <grub/dl.h>
#include <grub/net.h>
#include <grub/net/tcp.h>
#include <grub/net/netbuff.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/time.h>
#include <grub/term.h>

/* Defined in http.c, both objects are linked into the same http.mod.
   Every successful native stream pins its module independent of Lua/VFS. */
extern grub_dl_t grub_http_module_owner;

#define HTTP_IDLE_MS 30000
#define HTTP_HEADER_MAX 16384
#define HTTP_LINE_MAX 1024
#define HTTP_RX_MAX 131072
#define HTTP_BODY_MAX (8 * 1024 * 1024)
#define HTTP_BUFFER_SIZE 4096

struct grub_http_stream
{
  grub_dl_t owner;
  int secure;
  grub_tls_connection_t tls;
  grub_net_tcp_socket_t tcp;
  char *rx;
  grub_size_t rx_length, rx_offset;
  int broken, fin;
  unsigned char buffer[HTTP_BUFFER_SIZE];
  grub_size_t pos, length;
  unsigned status;
  char *headers;
  grub_uint64_t remaining;
  int known_length, chunked, ended, has_range;
  grub_uint64_t chunk_left, range_start, range_end, range_total;
};

static int
client_cancel (void)
{
  int key = grub_getkey_noblock ();
  if (key == 3 || key == 4 || grub_key_is_interrupt (key))
    {
      grub_error (GRUB_ERR_WAIT, "HTTP request interrupted");
      return 1;
    }
  return 0;
}

static int
tcp_cancel (void *unused __attribute__ ((unused)))
{
  return client_cancel ();
}

static grub_err_t
tcp_recv (grub_net_tcp_socket_t sock __attribute__ ((unused)),
          struct grub_net_buff *nb, void *context)
{
  grub_http_stream_t stream = context;
  grub_size_t unread = stream->rx_length - stream->rx_offset;
  grub_size_t n = nb->tail - nb->data;
  char *next;
  if (n > HTTP_RX_MAX - unread)
    {
      stream->broken = 1;
      grub_netbuff_free (nb);
      return grub_error (GRUB_ERR_OUT_OF_RANGE, "HTTP TCP receive queue full");
    }
  next = grub_malloc (unread + n + 1);
  if (!next)
    {
      stream->broken = 1;
      grub_netbuff_free (nb);
      return grub_errno;
    }
  if (unread)
    grub_memcpy (next, stream->rx + stream->rx_offset, unread);
  if (n)
    grub_memcpy (next + unread, nb->data, n);
  grub_free (stream->rx);
  stream->rx = next;
  stream->rx_length = unread + n;
  stream->rx_offset = 0;
  /* Respect GRUB TCP's advertised receive window.  Without stalling,
     a fast server can fill our bounded buffer before the reader is run. */
  if (stream->tcp && stream->rx_length >= 32768)
    grub_net_tcp_stall (stream->tcp);
  grub_netbuff_free (nb);
  return GRUB_ERR_NONE;
}

static void
tcp_error (grub_net_tcp_socket_t unused __attribute__ ((unused)),
           void *context)
{
  ((grub_http_stream_t) context)->broken = 1;
}

static void
tcp_fin (grub_net_tcp_socket_t unused __attribute__ ((unused)), void *context)
{
  ((grub_http_stream_t) context)->fin = 1;
}

static grub_ssize_t
transport_read (grub_http_stream_t stream, void *data, grub_size_t size)
{
  grub_uint64_t started = grub_get_time_ms ();
  if (stream->secure)
    return grub_tls_read (stream->tls, data, size);
  for (;;)
    {
      grub_size_t n = stream->rx_length - stream->rx_offset;
      if (n)
        {
          if (n > size) n = size;
          grub_memcpy (data, stream->rx + stream->rx_offset, n);
          stream->rx_offset += n;
          if (stream->rx_offset == stream->rx_length)
            {
              grub_free (stream->rx);
              stream->rx = NULL;
              stream->rx_offset = stream->rx_length = 0;
            }
          if (stream->tcp && stream->rx_length - stream->rx_offset < 16384)
            grub_net_tcp_unstall (stream->tcp);
          return n;
        }
      if (stream->broken)
        return grub_error (GRUB_ERR_IO, "HTTP TCP connection failed"), -1;
      if (stream->fin)
        return 0;
      if (client_cancel ()) return -1;
      if (grub_get_time_ms () - started >= HTTP_IDLE_MS)
        return grub_error (GRUB_ERR_TIMEOUT, "HTTP TCP timeout"), -1;
      grub_net_poll_cards (25, NULL);
    }
}

static grub_ssize_t
transport_write (grub_http_stream_t stream, const void *data, grub_size_t len)
{
  struct grub_net_buff *nb;
  grub_size_t n = len > 16384 ? 16384 : len;
  grub_err_t err;
  if (stream->secure)
    return grub_tls_write (stream->tls, data, n);
  nb = grub_netbuff_alloc (GRUB_NET_TCP_RESERVE_SIZE + n);
  if (!nb) return -1;
  if (grub_netbuff_reserve (nb, GRUB_NET_TCP_RESERVE_SIZE) ||
      grub_netbuff_put (nb, n))
    {
      grub_netbuff_free (nb);
      return -1;
    }
  grub_memcpy (nb->data, data, n);
  err = grub_net_send_tcp_packet (stream->tcp, nb, 1);
  return err ? -1 : (grub_ssize_t) n;
}

static grub_err_t
send_all (grub_http_stream_t stream, const void *ptr, grub_size_t len)
{
  const char *data = ptr;
  grub_size_t offset = 0;
  grub_ssize_t n;
  while (offset < len)
    {
      if (client_cancel ()) return grub_errno;
      n = transport_write (stream, data + offset, len - offset);
      if (n <= 0)
        return grub_error (GRUB_ERR_WRITE_ERROR, "HTTP request transmission failed");
      offset += n;
    }
  return GRUB_ERR_NONE;
}

static grub_ssize_t
reader_read (grub_http_stream_t stream, void *dst, grub_size_t wanted)
{
  grub_size_t n;
  grub_ssize_t got;
  if (!wanted) return 0;
  if (stream->pos == stream->length)
    {
      got = transport_read (stream, stream->buffer, HTTP_BUFFER_SIZE);
      if (got <= 0) return got;
      stream->pos = 0;
      stream->length = got;
    }
  n = stream->length - stream->pos;
  if (n > wanted) n = wanted;
  grub_memcpy (dst, stream->buffer + stream->pos, n);
  stream->pos += n;
  return n;
}

static grub_err_t
reader_exact (grub_http_stream_t stream, void *dst, grub_size_t wanted)
{
  grub_size_t pos = 0;
  grub_ssize_t n;
  while (pos < wanted)
    {
      n = reader_read (stream, (char *) dst + pos, wanted - pos);
      if (n <= 0)
        return n < 0 ? grub_errno :
          grub_error (GRUB_ERR_READ_ERROR, "truncated HTTP framing");
      pos += n;
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
reader_line (grub_http_stream_t stream, char *line, grub_size_t limit)
{
  grub_size_t used = 0;
  char c;
  grub_ssize_t n;
  while (used + 1 < limit)
    {
      n = reader_read (stream, &c, 1);
      if (n <= 0)
        return n < 0 ? grub_errno :
          grub_error (GRUB_ERR_READ_ERROR, "truncated HTTP line");
      if (c == '\n')
        {
          if (!used || line[used - 1] != '\r')
            return grub_error (GRUB_ERR_BAD_FILE_TYPE, "HTTP requires CRLF");
          line[used - 1] = 0;
          return GRUB_ERR_NONE;
        }
      line[used++] = c;
    }
  return grub_error (GRUB_ERR_OUT_OF_RANGE, "HTTP header line too long");
}

static int
header_match (const char *line, const char *name, const char **value)
{
  grub_size_t len = grub_strlen (name);
  if (!grub_strncasecmp (line, name, len) && line[len] == ':')
    {
      *value = line + len + 1;
      while (**value == ' ' || **value == '\t') (*value)++;
      return 1;
    }
  return 0;
}

static grub_err_t
parse_uint (const char *text, unsigned base, grub_uint64_t *out,
            int allow_extension)
{
  grub_uint64_t n = 0, limit = ~(grub_uint64_t) 0;
  int count = 0, digit;
  for (; *text; text++)
    {
      if (allow_extension && *text == ';') break;
      if (*text >= '0' && *text <= '9') digit = *text - '0';
      else if (base == 16 && *text >= 'a' && *text <= 'f')
        digit = *text - 'a' + 10;
      else if (base == 16 && *text >= 'A' && *text <= 'F')
        digit = *text - 'A' + 10;
      else
        return grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid HTTP integer");
      if (n > grub_divmod64 (limit - digit, base, NULL))
        return grub_error (GRUB_ERR_OUT_OF_RANGE, "HTTP integer overflow");
      n = n * base + digit;
      count++;
    }
  if (!count) return grub_error (GRUB_ERR_BAD_FILE_TYPE, "empty HTTP integer");
  *out = n;
  return GRUB_ERR_NONE;
}

static grub_err_t
parse_url (const char *url, char **host, char **path,
           grub_uint16_t *port, int *secure)
{
  const char *authority, *slash, *end, *sep;
  grub_size_t hostlen;
  grub_uint64_t parsed;
  if (!url) return grub_error (GRUB_ERR_BAD_ARGUMENT, "HTTP URL missing");
  if (!grub_strncmp (url, "https://", 8))
    { *secure = 1; authority = url + 8; }
  else if (!grub_strncmp (url, "http://", 7))
    { *secure = 0; authority = url + 7; }
  else
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP URL scheme");
  slash = grub_strchr (authority, '/');
  end = slash ? slash : authority + grub_strlen (authority);
  *path = grub_strdup (slash ? slash : "/");
  if (!*path) return grub_errno;
  if (authority == end || grub_memchr (authority, '@', end - authority))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP URL authority");
  {
    const char *check;
    for (check = authority; *check; check++)
      if ((unsigned char) *check <= 32 || (unsigned char) *check == 127)
        return grub_error (GRUB_ERR_BAD_ARGUMENT,
                           "control or whitespace in HTTP URL");
  }
  sep = NULL;
  if (*authority == '[')
    {
      const char *close = grub_memchr (authority, ']', end - authority);
      if (!close)
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid IPv6 URL host");
      if (close + 1 < end)
        {
          if (close[1] != ':')
            return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP URL port");
          sep = close + 1;
        }
    }
  else
    sep = grub_memchr (authority, ':', end - authority);
  hostlen = sep ? sep - authority : end - authority;
  *host = grub_strndup (authority, hostlen);
  if (!*host) return grub_errno;
  *port = *secure ? 443 : 80;
  if (sep)
    {
      char *port_text = grub_strndup (sep + 1, end - sep - 1);
      grub_err_t err;
      if (!port_text) return grub_errno;
      err = parse_uint (port_text, 10, &parsed, 0);
      grub_free (port_text);
      if (err) return err;
      if (!parsed || parsed > 65535)
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP port");
      *port = parsed;
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
parse_range_value (const char *text, grub_uint64_t *start,
                   grub_uint64_t *last, grub_uint64_t *total)
{
  const char *dash, *slash;
  char span[32];
  grub_size_t n;
  if (grub_strncmp (text, "bytes ", 6))
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid HTTP Content-Range");
  text += 6;
  dash = grub_strchr (text, '-');
  slash = dash ? grub_strchr (dash + 1, '/') : NULL;
  if (!slash || slash <= dash + 1 || !slash[1] || slash[1] == '*')
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "incomplete HTTP Content-Range");
  n = dash - text;
  if (!n || n >= sizeof (span))
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid HTTP range start");
  grub_memcpy (span, text, n);
  span[n] = 0;
  if (parse_uint (span, 10, start, 0)) return grub_errno;
  n = slash - dash - 1;
  if (!n || n >= sizeof (span))
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid HTTP range end");
  grub_memcpy (span, dash + 1, n);
  span[n] = 0;
  if (parse_uint (span, 10, last, 0) ||
      parse_uint (slash + 1, 10, total, 0)) return grub_errno;
  if (*start > *last || *last >= *total)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid HTTP range bounds");
  return GRUB_ERR_NONE;
}

static grub_err_t
validate_header (const char *name, const char *value)
{
  const char *p;
  if (!name || !*name || !value)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "empty HTTP header");
  for (p = name; *p; p++)
    if (*p <= ' ' || *p == ':' || *p >= 127)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP header name");
  for (p = value; *p; p++)
    if (*p == '\r' || *p == '\n' || (unsigned char) *p < 32)
      return grub_error (GRUB_ERR_BAD_ARGUMENT, "unsafe HTTP header value");
  if (!grub_strcasecmp (name, "Host") ||
      !grub_strcasecmp (name, "Connection") ||
      !grub_strcasecmp (name, "Content-Length") ||
      !grub_strcasecmp (name, "Transfer-Encoding"))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "reserved HTTP header");
  return GRUB_ERR_NONE;
}

void
grub_http_stream_close (grub_http_stream_t stream)
{
  grub_err_t saved = grub_errno;
  grub_dl_t owner;
  if (!stream) return;
  owner = stream->owner;
  if (stream->tls) grub_tls_close (stream->tls);
  if (stream->tcp)
    {
      grub_net_tcp_close (stream->tcp, GRUB_NET_TCP_ABORT);
      grub_net_tcp_dispose (stream->tcp);
    }
  grub_free (stream->rx);
  grub_free (stream->headers);
  grub_free (stream);
  if (owner) grub_dl_unref (owner);
  grub_errno = saved;
}

unsigned
grub_http_stream_status (grub_http_stream_t stream)
{
  return stream ? stream->status : 0;
}

const char *
grub_http_stream_headers (grub_http_stream_t stream)
{
  return stream ? stream->headers : NULL;
}

int
grub_http_stream_length (grub_http_stream_t stream, grub_uint64_t *length)
{
  if (!stream || (!stream->known_length && !stream->has_range)) return 0;
  if (length)
    *length = stream->has_range ?
      stream->range_total - stream->range_start : stream->remaining;
  return 1;
}

static grub_err_t
parse_response (grub_http_stream_t stream)
{
  char line[HTTP_LINE_MAX];
  const char *value;
  grub_size_t total = 0;
  grub_uint64_t number;
  int chunked = 0, length_seen = 0;
  char *next;
  if (reader_line (stream, line, sizeof (line))) return grub_errno;
  if (grub_strlen (line) < 12 ||
      grub_strncmp (line, "HTTP/1.", 7) || line[8] != ' ' ||
      line[9] < '0' || line[9] > '9' ||
      line[10] < '0' || line[10] > '9' ||
      line[11] < '0' || line[11] > '9')
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid HTTP status");
  stream->status = (line[9] - '0') * 100 +
                   (line[10] - '0') * 10 + line[11] - '0';
  stream->headers = grub_strdup (line);
  if (!stream->headers) return grub_errno;
  total = grub_strlen (line);
  for (;;)
    {
      grub_size_t size;
      if (reader_line (stream, line, sizeof (line))) return grub_errno;
      if (!*line) break;
      size = grub_strlen (line);
      if (total + size + 3 > HTTP_HEADER_MAX)
        return grub_error (GRUB_ERR_OUT_OF_RANGE, "HTTP headers exceed limit");
      next = grub_realloc (stream->headers, total + size + 3);
      if (!next) return grub_errno;
      stream->headers = next;
      grub_memcpy (next + total, "\r\n", 2);
      grub_memcpy (next + total + 2, line, size + 1);
      total += size + 2;
      if (header_match (line, "Transfer-Encoding", &value))
        {
          if (grub_strcasecmp (value, "chunked"))
            return grub_error (GRUB_ERR_BAD_FILE_TYPE, "unsupported HTTP encoding");
          chunked = 1;
        }
      else if (header_match (line, "Content-Length", &value))
        {
          if (parse_uint (value, 10, &number, 0)) return grub_errno;
          if (length_seen && stream->remaining != number)
            return grub_error (GRUB_ERR_BAD_FILE_TYPE, "conflicting Content-Length");
          stream->remaining = number;
          length_seen = 1;
        }
      else if (header_match (line, "Content-Range", &value))
        {
          if (stream->has_range ||
              parse_range_value (value, &stream->range_start,
                                 &stream->range_end, &stream->range_total))
            return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                               "invalid or duplicate HTTP Content-Range");
          stream->has_range = 1;
        }
      else if (header_match (line, "Content-Encoding", &value))
        {
          if (grub_strcasecmp (value, "identity"))
            return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                               "unsupported HTTP content encoding");
        }
    }
  if (chunked && length_seen)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                       "ambiguous HTTP Transfer-Encoding and Content-Length");
  if (stream->has_range && length_seen &&
      (stream->range_end - stream->range_start + 1 != stream->remaining))
    return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                       "HTTP Content-Range length mismatch");
  stream->chunked = chunked;
  stream->known_length = length_seen && !chunked;
  if (stream->status == 204 || stream->status == 304 ||
      (stream->status >= 100 && stream->status < 200))
    stream->ended = 1;
  return GRUB_ERR_NONE;
}

grub_http_stream_t
grub_http_stream_open (const struct grub_http_request *req,
                       grub_uint64_t range_start)
{
  grub_http_stream_t stream = NULL;
  char *host = NULL, *path = NULL, *first = NULL, *host_header = NULL;
  grub_uint16_t port = 0;
  grub_size_t i, capacity;
  grub_err_t err;
  int secure = 0;
  const char *method;
  if (!req || !req->url)
    { grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP request"); return NULL; }
  err = parse_url (req->url, &host, &path, &port, &secure);
  if (err) goto failed;
  method = req->method ? req->method : "GET";
  if (grub_strcmp (method, "GET") && grub_strcmp (method, "POST") &&
      grub_strcmp (method, "PUT") && grub_strcmp (method, "PATCH") &&
      grub_strcmp (method, "DELETE") && grub_strcmp (method, "HEAD"))
    { grub_error (GRUB_ERR_BAD_ARGUMENT, "unsupported HTTP method"); goto failed; }
  if ((req->body_length && !req->body) || req->header_count > 32)
    { grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP body or headers"); goto failed; }
  for (i = 0; i < req->header_count; i++)
    if (validate_header (req->headers[i].name, req->headers[i].value))
      goto failed;
  stream = grub_zalloc (sizeof (*stream));
  if (!stream) goto failed;
  stream->secure = secure;
  if (secure)
    {
      stream->tls = grub_tls_connect (host, port, NULL);
      if (!stream->tls) goto failed;
    }
  else
    {
      stream->tcp = grub_net_tcp_open_cancelable (host, port,
                                  tcp_recv, tcp_error, tcp_fin, stream,
                                  tcp_cancel, NULL);
      if (!stream->tcp) goto failed;
    }
  host_header = (port == (secure ? 443 : 80)) ?
    grub_strdup (host) : grub_xasprintf ("%s:%u", host, port);
  if (!host_header) goto failed;
  capacity = grub_strlen (host_header) + grub_strlen (path) +
             grub_strlen (method) + 256;
  first = grub_malloc (capacity);
  if (!first) goto failed;
  grub_snprintf (first, capacity, "%s %s HTTP/1.1\r\nHost: %s\r\n"
                 "User-Agent: GRUB/2\r\nConnection: close\r\n"
                 "Content-Length: %lu\r\n", method, path, host_header,
                 (unsigned long) req->body_length);
  if (send_all (stream, first, grub_strlen (first))) goto failed;
  if (range_start)
    {
      char range[80];
      grub_snprintf (range, sizeof (range), "Range: bytes=%llu-\r\n",
                     (unsigned long long) range_start);
      if (send_all (stream, range, grub_strlen (range))) goto failed;
    }
  for (i = 0; i < req->header_count; i++)
    {
      const struct grub_http_header *header = &req->headers[i];
      if (send_all (stream, header->name, grub_strlen (header->name)) ||
          send_all (stream, ": ", 2) ||
          send_all (stream, header->value, grub_strlen (header->value)) ||
          send_all (stream, "\r\n", 2)) goto failed;
    }
  if (send_all (stream, "\r\n", 2)) goto failed;
  if (req->body_length &&
      send_all (stream, req->body, req->body_length)) goto failed;
  if (parse_response (stream)) goto failed;
  if (range_start && (stream->status != 206 ||
                      !stream->has_range ||
                      stream->range_start != range_start ||
                      stream->range_end + 1 != stream->range_total))
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE,
                  "HTTP server returned incorrect Range response");
      goto failed;
    }
  if (!grub_strcmp (method, "HEAD")) stream->ended = 1;
  grub_dl_ref (grub_http_module_owner);
  stream->owner = grub_http_module_owner;
  grub_free (host);
  grub_free (path);
  grub_free (first);
  grub_free (host_header);
  return stream;
failed:
  grub_http_stream_close (stream);
  grub_free (host);
  grub_free (path);
  grub_free (first);
  grub_free (host_header);
  return NULL;
}

grub_ssize_t
grub_http_stream_read (grub_http_stream_t stream, void *dst, grub_size_t size)
{
  grub_ssize_t n;
  grub_uint64_t chunk;
  char line[HTTP_LINE_MAX];
  char trailer[2];
  if (!stream || (!dst && size))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid HTTP read"), -1;
  if (!size || stream->ended) return 0;
  if (stream->chunked)
    {
      if (!stream->chunk_left)
        {
          if (reader_line (stream, line, sizeof (line)) ||
              parse_uint (line, 16, &chunk, 1))
            return -1;
          if (!chunk)
            {
              do
                {
                  if (reader_line (stream, line, sizeof (line))) return -1;
                }
              while (*line);
              stream->ended = 1;
              return 0;
            }
          stream->chunk_left = chunk;
        }
      if (size > stream->chunk_left) size = stream->chunk_left;
    }
  else if (stream->known_length)
    {
      if (!stream->remaining) { stream->ended = 1; return 0; }
      if (size > stream->remaining) size = stream->remaining;
    }
  n = reader_read (stream, dst, size);
  if (n < 0) return -1;
  if (!n)
    {
      if (!stream->known_length && !stream->chunked)
        { stream->ended = 1; return 0; }
      return grub_error (GRUB_ERR_READ_ERROR, "truncated HTTP body"), -1;
    }
  if (stream->known_length) stream->remaining -= n;
  if (stream->chunked)
    {
      stream->chunk_left -= n;
      if (!stream->chunk_left)
        {
          if (reader_exact (stream, trailer, 2)) return -1;
          if (trailer[0] != '\r' || trailer[1] != '\n')
            return grub_error (GRUB_ERR_BAD_FILE_TYPE, "bad HTTP chunk delimiter"), -1;
        }
    }
  return n;
}

void
grub_http_response_free (struct grub_http_response *res)
{
  if (!res) return;
  grub_free (res->headers);
  grub_free (res->body);
  grub_memset (res, 0, sizeof (*res));
}

grub_err_t
grub_http_request (const struct grub_http_request *req,
                   struct grub_http_response *response)
{
  grub_http_stream_t stream;
  grub_size_t size = 0, capacity = 0;
  grub_ssize_t n;
  char *next;
  if (!response)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "null HTTP response");
  grub_memset (response, 0, sizeof (*response));
  stream = grub_http_stream_open (req, 0);
  if (!stream) return grub_errno;
  response->status = stream->status;
  response->headers = grub_strdup (stream->headers ? stream->headers : "");
  if (!response->headers) goto failed;
  for (;;)
    {
      if (capacity - size < HTTP_BUFFER_SIZE)
        {
          grub_size_t newcap = capacity ? capacity * 2 : 8192;
          if (newcap > HTTP_BODY_MAX + 1) newcap = HTTP_BODY_MAX + 1;
          if (newcap <= capacity)
            { grub_error (GRUB_ERR_OUT_OF_RANGE, "HTTP body too large"); goto failed; }
          next = grub_realloc (response->body, newcap);
          if (!next) goto failed;
          response->body = next;
          capacity = newcap;
        }
      n = grub_http_stream_read (stream, response->body + size, capacity - size - 1);
      if (n < 0) goto failed;
      if (!n) break;
      size += n;
      if (size > HTTP_BODY_MAX)
        { grub_error (GRUB_ERR_OUT_OF_RANGE, "HTTP body exceeds limit"); goto failed; }
    }
  response->body[size] = 0;
  response->body_length = size;
  grub_http_stream_close (stream);
  return GRUB_ERR_NONE;
failed:
  grub_http_stream_close (stream);
  grub_http_response_free (response);
  return grub_errno;
}
