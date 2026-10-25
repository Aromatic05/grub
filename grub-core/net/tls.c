/*
 * TLS 1.2 client transport for GRUB's TCP stack.
 *
 * TLS and X.509 are independent of HTTP and Lua. Script runtimes use the
 * existing native Binding ABI, without a TLS-specific Lua module.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <grub/tls.h>
#include <grub/bind.h>
#include <grub/command.h>
#include <grub/dl.h>
#include <grub/net.h>
#include <grub/net/tcp.h>
#include <grub/net/netbuff.h>
#include <grub/datetime.h>
#include <grub/env.h>
#include <grub/file.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/random.h>
#include <grub/time.h>
#include <grub/term.h>
#ifdef GRUB_MACHINE_EFI
#include <grub/efi/efi.h>
#include <grub/efi/api.h>
#endif

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define TLS_DEADLINE_MS 30000
#define TLS_RX_BUFFER_MAX (256 * 1024)
#define TLS_CA_MAX (512 * 1024)
#define TLS_RECORD_MAX 16384
#define TLS_SEED_ATTEMPTS 3
#define TLS_CLOSE_DEADLINE_MS 500

struct grub_tls_connection
{
  grub_net_tcp_socket_t sock;
  grub_uint8_t *received;
  grub_size_t length;
  grub_size_t cursor;
  int remote_eof;
  int failed;
  int owns_module_ref;
  int handshake_complete;
  int tls_eof;
  struct grub_datetime clock_utc;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt roots;
};

static grub_dl_t tls_module;
static grub_command_t tls_check_command;
static mbedtls_ctr_drbg_context shared_drbg;
static mbedtls_entropy_context shared_entropy;
static int drbg_initialized;
static int drbg_seeded;
static unsigned connected_count;

#ifdef GRUB_MACHINE_EFI
static grub_guid_t efi_rng_protocol_guid = GRUB_EFI_RNG_PROTOCOL_GUID;
#endif

static int
secure_entropy (void *context __attribute__ ((unused)), unsigned char *out,
                size_t size, size_t *produced)
{
  grub_err_t saved = grub_errno;
#ifdef GRUB_MACHINE_EFI
  grub_efi_rng_protocol_t *rng =
    grub_efi_locate_protocol (&efi_rng_protocol_guid, NULL);
  if (rng && rng->get_rng (rng, NULL, size, out) == GRUB_EFI_SUCCESS)
    {
      *produced = size;
      grub_errno = saved;
      return 0;
    }
#endif
  if (grub_crypto_get_random (out, size) == GRUB_ERR_NONE)
    {
      *produced = size;
      grub_errno = saved;
      return 0;
    }
  /* Never replace missing entropy with clock, addresses or an insecure PRNG. */
  *produced = 0;
  grub_errno = saved;
  return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}

static void
reset_random_state (void)
{
  if (drbg_initialized)
    {
      mbedtls_ctr_drbg_free (&shared_drbg);
      mbedtls_entropy_free (&shared_entropy);
    }
  drbg_initialized = drbg_seeded = 0;
}

static int
ensure_random_state (void)
{
  static const char personalization[] = "grub-native-tls";
  int ret;

  if (drbg_seeded)
    return 0;
  mbedtls_ctr_drbg_init (&shared_drbg);
  mbedtls_entropy_init (&shared_entropy);
  drbg_initialized = 1;
  ret = mbedtls_entropy_add_source (&shared_entropy, secure_entropy, NULL, 32,
                                    MBEDTLS_ENTROPY_SOURCE_STRONG);
  if (!ret)
    ret = mbedtls_ctr_drbg_seed (&shared_drbg, mbedtls_entropy_func,
                                  &shared_entropy,
                                  (const unsigned char *) personalization,
                                  sizeof (personalization) - 1);
  if (ret)
    {
      reset_random_state ();
      return ret;
    }
  drbg_seeded = 1;
  return 0;
}

static grub_err_t
receive_ciphertext (grub_net_tcp_socket_t unused __attribute__ ((unused)),
                    struct grub_net_buff *nb, void *userdata)
{
  grub_tls_connection_t connection = userdata;
  grub_size_t bytes = nb->tail - nb->data;
  grub_size_t waiting = connection->length - connection->cursor;
  grub_uint8_t *buffer;

  if (bytes > TLS_RX_BUFFER_MAX - waiting)
    {
      connection->failed = 1;
      grub_netbuff_free (nb);
      return grub_error (GRUB_ERR_OUT_OF_RANGE, "TLS ciphertext queue full");
    }

  if (!bytes)
    {
      grub_netbuff_free (nb);
      return GRUB_ERR_NONE;
    }
  buffer = grub_malloc (waiting + bytes);
  if (!buffer)
    {
      connection->failed = 1;
      grub_netbuff_free (nb);
      return grub_errno;
    }
  if (waiting)
    grub_memcpy (buffer, connection->received + connection->cursor, waiting);
  grub_memcpy (buffer + waiting, nb->data, bytes);
  grub_free (connection->received);
  connection->received = buffer;
  connection->length = waiting + bytes;
  connection->cursor = 0;
  grub_netbuff_free (nb);
  return GRUB_ERR_NONE;
}

static void
transport_failed (grub_net_tcp_socket_t unused __attribute__ ((unused)),
                  void *userdata)
{
  ((grub_tls_connection_t) userdata)->failed = 1;
}

static void
transport_eof (grub_net_tcp_socket_t unused __attribute__ ((unused)),
               void *userdata)
{
  ((grub_tls_connection_t) userdata)->remote_eof = 1;
}

/* Mbed TLS uses nonblocking callbacks. Public methods poll GRUB networking. */
static int
receive_record (void *userdata, unsigned char *dst, size_t capacity)
{
  grub_tls_connection_t conn = userdata;
  grub_size_t ready = conn->length - conn->cursor;
  if (ready)
    {
      if (capacity > ready)
        capacity = ready;
      grub_memcpy (dst, conn->received + conn->cursor, capacity);
      conn->cursor += capacity;
      if (conn->cursor == conn->length)
        {
          grub_free (conn->received);
          conn->received = NULL;
          conn->length = conn->cursor = 0;
        }
      return (int) capacity;
    }
  if (conn->failed)
    return MBEDTLS_ERR_NET_RECV_FAILED;
  /* TCP FIN without an authenticated TLS close_notify is truncation,
     not a clean EOF.  Returning zero would make Mbed TLS report success. */
  if (conn->remote_eof)
    return MBEDTLS_ERR_NET_RECV_FAILED;
  return MBEDTLS_ERR_SSL_WANT_READ;
}

static int
send_record (void *userdata, const unsigned char *src, size_t length)
{
  grub_tls_connection_t conn = userdata;
  struct grub_net_buff *packet;
  grub_err_t err;

  if (!conn->sock || conn->failed)
    return MBEDTLS_ERR_NET_SEND_FAILED;
  if (length > TLS_RECORD_MAX)
    length = TLS_RECORD_MAX;

  packet = grub_netbuff_alloc (GRUB_NET_TCP_RESERVE_SIZE + length);
  if (!packet)
    return MBEDTLS_ERR_NET_SEND_FAILED;
  err = grub_netbuff_reserve (packet, GRUB_NET_TCP_RESERVE_SIZE);
  if (err)
    {
      grub_netbuff_free (packet);
      return MBEDTLS_ERR_NET_SEND_FAILED;
    }
  err = grub_netbuff_put (packet, length);
  if (err)
    {
      grub_netbuff_free (packet);
      return MBEDTLS_ERR_NET_SEND_FAILED;
    }
  grub_memcpy (packet->data, src, length);
  err = grub_net_send_tcp_packet (conn->sock, packet, 1);
  return err ? MBEDTLS_ERR_NET_SEND_FAILED : (int) length;
}

/* Request cancellation is distinct from a TLS alert or TCP shutdown.
   GRUB consoles expose Ctrl-C/D as either ASCII controls or modified keys.
   ESC/F4 also use GRUB's existing interrupt convention. */
static int
tls_cancel_requested (void)
{
  int key = grub_getkey_noblock ();
  if (key == 3 || key == 4 ||
      key == (GRUB_TERM_CTRL | 'c') ||
      key == (GRUB_TERM_CTRL | 'C') ||
      key == (GRUB_TERM_CTRL | 'd') ||
      key == (GRUB_TERM_CTRL | 'D') ||
      grub_key_is_interrupt (key))
    {
      grub_error (GRUB_ERR_WAIT, "TLS operation interrupted");
      return 1;
    }
  return 0;
}

static int
tls_cancel_tcp_open (void *unused __attribute__ ((unused)))
{
  return tls_cancel_requested ();
}

/* UEFI TimeZone is UTC minus local time (minutes); daylight adjustments
   are already reflected in the firmware's reported TimeZone.  On systems
   without timezone metadata, an explicit grub_tls_utc_offset can supply
   the same UTC-minus-RTC offset.  Otherwise the RTC is assumed UTC,
   as was previously the case on BIOS and timezone-unspecified firmware. */
static grub_err_t
tls_get_utc_time (struct grub_datetime *utc)
{
  const char *override = grub_env_get ("grub_tls_utc_offset");
  grub_int64_t seconds;
  long offset = 0;
#ifdef GRUB_MACHINE_EFI
  grub_efi_time_t firmware;
  if (grub_efi_system_table->runtime_services->get_time (&firmware, NULL)
      != GRUB_EFI_SUCCESS)
    return grub_error (GRUB_ERR_BAD_FIRMWARE, "cannot read TLS certificate clock");
  utc->year = firmware.year;
  utc->month = firmware.month;
  utc->day = firmware.day;
  utc->hour = firmware.hour;
  utc->minute = firmware.minute;
  utc->second = firmware.second;
  if (firmware.time_zone != 0x07ff)
    {
      if (firmware.time_zone < -1440 || firmware.time_zone > 1440)
        return grub_error (GRUB_ERR_BAD_FIRMWARE, "invalid EFI timezone");
      offset = firmware.time_zone;
    }
#else
  if (grub_get_datetime (utc))
    return grub_errno;
#endif
  if (override && *override)
    {
      const char *end;
      grub_errno = GRUB_ERR_NONE;
      offset = grub_strtol (override, &end, 10);
      if (grub_errno || *end || offset < -1440 || offset > 1440)
        return grub_error (GRUB_ERR_BAD_ARGUMENT,
                           "invalid grub_tls_utc_offset minutes");
    }
  if (!grub_datetime2unixtime (utc, &seconds))
    return grub_error (GRUB_ERR_BAD_FIRMWARE, "invalid TLS certificate clock");
  seconds += (grub_int64_t) offset * 60;
  grub_unixtime2datetime (seconds, utc);
  return GRUB_ERR_NONE;
}

/* Verify validity dates against GRUB's firmware clock, since this freestanding
   Mbed TLS build does not have libc time()/gmtime(). Clock failure is fatal. */
static int
compare_certificate_date (const mbedtls_x509_time *cert,
                          const struct grub_datetime *now)
{
  int left[6] = { cert->year, cert->mon, cert->day,
                  cert->hour, cert->min, cert->sec };
  int right[6] = { now->year, now->month, now->day,
                   now->hour, now->minute, now->second };
  unsigned i;
  for (i = 0; i < 6; ++i)
    if (left[i] != right[i])
      return left[i] < right[i] ? -1 : 1;
  return 0;
}

static int
verify_certificate_clock (void *ctx __attribute__ ((unused)),
                          mbedtls_x509_crt *cert,
                          int depth __attribute__ ((unused)), uint32_t *flags)
{
  const grub_tls_connection_t conn = ctx;
  if (compare_certificate_date (&cert->valid_from, &conn->clock_utc) > 0)
    *flags |= MBEDTLS_X509_BADCERT_FUTURE;
  if (compare_certificate_date (&cert->valid_to, &conn->clock_utc) < 0)
    *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
  return 0;
}

static grub_err_t
read_trust_anchors (grub_tls_connection_t conn, const char *name)
{
  grub_file_t source;
  grub_off_t size;
  grub_ssize_t bytes;
  unsigned char *pem;
  int ret;

  if (!name || !*name)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "TLS requires a PEM CA path (grub_tls_ca)");
  source = grub_file_open (name, GRUB_FILE_TYPE_NONE);
  if (!source)
    return grub_errno;
  size = grub_file_size (source);
  if (size <= 0 || size > TLS_CA_MAX)
    {
      grub_file_close (source);
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, "invalid TLS CA file size");
    }

  pem = grub_malloc ((grub_size_t) size + 1);
  if (!pem)
    {
      grub_file_close (source);
      return grub_errno;
    }
  bytes = grub_file_read (source, pem, (grub_size_t) size);
  grub_file_close (source);
  if (bytes < 0 || (grub_uint64_t) bytes != (grub_uint64_t) size)
    {
      grub_free (pem);
      return grub_error (GRUB_ERR_READ_ERROR, "cannot read TLS CA file");
    }
  pem[size] = 0;
  ret = mbedtls_x509_crt_parse (&conn->roots, pem, size + 1);
  grub_memset (pem, 0, size + 1);
  grub_free (pem);
  /* Positive return means some unrelated bundle entries were rejected. Only
     successfully parsed certificates are trusted; verification stays strict. */
  if (ret < 0 || !conn->roots.raw.p)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE,
                       "cannot parse trusted TLS certificates (%d)", ret);
  return GRUB_ERR_NONE;
}

void
grub_tls_close (grub_tls_connection_t conn)
{
  grub_err_t original = grub_errno;
  if (!conn)
    return;
  if (conn->sock)
    {
      /* Best-effort authenticated shutdown before TCP FIN.  Do not wait
         indefinitely for peer progress or an unavailable network card. */
      if (conn->handshake_complete && !conn->failed)
        {
          int status;
          grub_uint64_t started = grub_get_time_ms ();
          do
            {
              status = mbedtls_ssl_close_notify (&conn->ssl);
              if (status != MBEDTLS_ERR_SSL_WANT_READ &&
                  status != MBEDTLS_ERR_SSL_WANT_WRITE)
                break;
              grub_net_poll_cards (50, NULL);
            }
          while (grub_get_time_ms () - started < TLS_CLOSE_DEADLINE_MS);
        }
      grub_net_tcp_close (conn->sock, conn->handshake_complete ?
                          GRUB_NET_TCP_DISCARD : GRUB_NET_TCP_ABORT);
      /* close() alone leaves the socket registered and retains buffers. */
      grub_net_tcp_dispose (conn->sock);
      conn->sock = NULL;
    }
  mbedtls_ssl_free (&conn->ssl);
  mbedtls_ssl_config_free (&conn->conf);
  mbedtls_x509_crt_free (&conn->roots);
  grub_free (conn->received);
  if (conn->owns_module_ref)
    {
      connected_count--;
      grub_dl_unref (tls_module);
    }
  grub_memset (conn, 0, sizeof (*conn));
  grub_free (conn);
  grub_errno = original;
}

static grub_tls_connection_t
connect_once (const char *host, grub_uint16_t port, const char *ca,
              int *entropy_error)
{
  grub_tls_connection_t conn;
  grub_uint64_t started;
  int ret;

  conn = grub_zalloc (sizeof (*conn));
  if (!conn)
    return NULL;
  mbedtls_ssl_init (&conn->ssl);
  mbedtls_ssl_config_init (&conn->conf);
  mbedtls_x509_crt_init (&conn->roots);

  if (read_trust_anchors (conn, ca))
    goto failed;
  if (tls_get_utc_time (&conn->clock_utc))
    goto failed;
  ret = ensure_random_state ();
  if (ret)
    goto crypto_error;
  ret = mbedtls_ssl_config_defaults (&conn->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret)
    goto crypto_error;
  mbedtls_ssl_conf_rng (&conn->conf, mbedtls_ctr_drbg_random, &shared_drbg);
  mbedtls_ssl_conf_ca_chain (&conn->conf, &conn->roots, NULL);
  mbedtls_ssl_conf_authmode (&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_verify (&conn->conf, verify_certificate_clock, conn);
  ret = mbedtls_ssl_setup (&conn->ssl, &conn->conf);
  if (ret)
    goto crypto_error;
  ret = mbedtls_ssl_set_hostname (&conn->ssl, host);
  if (ret)
    goto crypto_error;

  conn->sock = grub_net_tcp_open_cancelable ((char *) host,
                                             port ? port : 443,
                                             receive_ciphertext,
                                             transport_failed,
                                             transport_eof, conn,
                                             tls_cancel_tcp_open, NULL);
  if (!conn->sock)
    goto failed;
  mbedtls_ssl_set_bio (&conn->ssl, conn, send_record, receive_record, NULL);

  started = grub_get_time_ms ();
  do
    {
      ret = mbedtls_ssl_handshake (&conn->ssl);
      if (!ret)
        {
          grub_dl_ref (tls_module);
          conn->handshake_complete = 1;
          conn->owns_module_ref = 1;
          connected_count++;
          return conn;
        }
      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        goto crypto_error;
      if (conn->failed || conn->remote_eof)
        {
          grub_error (GRUB_ERR_NET_NO_ROUTE, "TLS TCP connection closed");
          goto failed;
        }
      grub_net_poll_cards (50, NULL);
      if (tls_cancel_requested ())
        goto failed;
    }
  while (grub_get_time_ms () - started < TLS_DEADLINE_MS);

  grub_error (GRUB_ERR_TIMEOUT, "TLS handshake timeout");
  goto failed;

crypto_error:
  if (ret == MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED ||
      ret == MBEDTLS_ERR_ENTROPY_SOURCE_FAILED)
    {
      *entropy_error = 1;
      grub_error (GRUB_ERR_IO, "TLS secure entropy source failed (%d)", ret);
    }
  else if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
    grub_error (GRUB_ERR_ACCESS_DENIED,
                "TLS certificate verification failed (%d flags=0x%x)", ret,
                (unsigned) mbedtls_ssl_get_verify_result (&conn->ssl));
  else
    grub_error (GRUB_ERR_ACCESS_DENIED,
                "TLS handshake failed (%d)", ret);
failed:
  grub_tls_close (conn);
  return NULL;
}

grub_tls_connection_t
grub_tls_connect (const char *host, grub_uint16_t port, const char *ca_path)
{
  unsigned retry;
  const char *trust = ca_path ? ca_path : grub_env_get ("grub_tls_ca");
  if (!host || !*host)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "TLS hostname missing");
      return NULL;
    }
  /* Only retry entropy failures, before any application data is sent. No
     verification failure or already-issued request is silently retried. */
  for (retry = 0; retry < TLS_SEED_ATTEMPTS; retry++)
    {
      int entropy_error = 0;
      grub_tls_connection_t conn = connect_once (host, port, trust,
                                                 &entropy_error);
      if (conn || !entropy_error || connected_count)
        return conn;
      reset_random_state ();
      if (retry + 1 < TLS_SEED_ATTEMPTS)
        {
          grub_errno = GRUB_ERR_NONE;
          grub_net_poll_cards (50, NULL);
        }
    }
  return NULL;
}

grub_ssize_t
grub_tls_write (grub_tls_connection_t conn, const void *buffer,
                grub_size_t length)
{
  const unsigned char *src = buffer;
  grub_size_t sent = 0;
  grub_uint64_t progress = grub_get_time_ms ();
  int ret = 0;
  if (!conn || (!buffer && length))
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid TLS write buffer");
      return -1;
    }
  if (conn->failed || conn->tls_eof)
    {
      grub_error (GRUB_ERR_WRITE_ERROR, "TLS connection is no longer writable");
      return -1;
    }
  while (sent < length)
    {
      grub_size_t size = length - sent;
      if (size > TLS_RECORD_MAX) size = TLS_RECORD_MAX;
      ret = mbedtls_ssl_write (&conn->ssl, src + sent, size);
      if (ret > 0)
        {
          sent += ret;
          progress = grub_get_time_ms ();
          continue;
        }
      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        break;
      if (conn->failed || conn->remote_eof ||
          grub_get_time_ms () - progress >= TLS_DEADLINE_MS)
        break;
      grub_net_poll_cards (50, NULL);
      if (tls_cancel_requested ())
        {
          conn->failed = 1;
          return -1;
        }
    }
  if (sent == length)
    return sent;
  /* Bytes may already have been transmitted.  Never permit a retry on
     this session to accidentally duplicate a partially delivered record. */
  conn->failed = 1;
  grub_error (GRUB_ERR_WRITE_ERROR, "TLS write error (%d)", ret);
  return -1;
}

grub_ssize_t
grub_tls_read (grub_tls_connection_t conn, void *buffer, grub_size_t length)
{
  grub_uint64_t start = grub_get_time_ms ();
  int ret = 0;
  if (!conn || (!buffer && length))
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid TLS read buffer");
      return -1;
    }
  if (!length) return 0;
  if (conn->tls_eof) return 0;
  if (conn->failed)
    {
      grub_error (GRUB_ERR_READ_ERROR, "TLS connection has failed");
      return -1;
    }
  for (;;)
    {
      ret = mbedtls_ssl_read (&conn->ssl, buffer, length);
      if (ret > 0)
        return ret;
      if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
          conn->tls_eof = 1;
          return 0;
        }
      if (ret == 0)
        break; /* No authenticated close_notify was observed. */
      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        break;
      if (conn->failed || conn->remote_eof ||
          grub_get_time_ms () - start >= TLS_DEADLINE_MS)
        break;
      grub_net_poll_cards (50, NULL);
      if (tls_cancel_requested ())
        {
          conn->failed = 1;
          return -1;
        }
    }
  conn->failed = 1;
  if (conn->remote_eof || ret == 0)
    grub_error (GRUB_ERR_READ_ERROR,
                "TLS stream truncated without close_notify");
  else
    grub_error (GRUB_ERR_READ_ERROR, "TLS read error (%d)", ret);
  return -1;
}

/* Native CLI: a verified handshake without requiring a scripting runtime. */
static grub_err_t
tls_check (grub_command_t command __attribute__ ((unused)),
           int argc, char **args)
{
  grub_tls_connection_t conn;
  grub_uint16_t port = 443;
  if (argc < 1 || argc > 3)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "expected HOST [PORT [CA_FILE]]");
  if (argc >= 2)
    {
      const char *end;
      unsigned long val = grub_strtoul (args[1], &end, 10);
      if (grub_errno || *end || val < 1 || val > 65535)
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid TLS port");
      port = val;
    }
  conn = grub_tls_connect (args[0], port, argc >= 3 ? args[2] : NULL);
  if (!conn)
    return grub_errno;
  grub_printf ("TLS: %s (%s), peer verified\n",
               mbedtls_ssl_get_version (&conn->ssl),
               mbedtls_ssl_get_ciphersuite (&conn->ssl));
  grub_tls_close (conn);
  return GRUB_ERR_NONE;
}

static void
close_native_connection (void *data)
{
  grub_tls_close (data);
}

static grub_err_t
bind_connect (grub_size_t argc, const struct grub_bind_value *argv,
              struct grub_bind_value *out, grub_size_t *count)
{
  grub_uint64_t requested = argc > 1 ? argv[1].value.u64 : 443;
  const char *ca = argc > 2 ? argv[2].value.bytes.data : NULL;
  grub_tls_connection_t conn;
  struct grub_bind_handle *handle;

  if (requested < 1 || requested > 65535)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid TLS port");
  /* Native consumers supply NUL-terminated names; the Binding ABI also
     carries lengths, so reject embedded NULs rather than shortening names
     or CA paths without the caller's knowledge. */
  if (grub_strlen (argv[0].value.bytes.data) != argv[0].value.bytes.size ||
      (argc > 2 &&
       grub_strlen (ca) != argv[2].value.bytes.size))
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       "embedded NUL in TLS host or CA path");
  conn = grub_tls_connect (argv[0].value.bytes.data, requested, ca);
  if (!conn)
    return grub_errno;
  handle = grub_bind_handle_create (tls_module, "tls.connection", conn,
                                    close_native_connection);
  if (!handle)
    {
      grub_tls_close (conn);
      return grub_errno;
    }
  out[0].type = GRUB_BIND_HANDLE;
  out[0].value.handle = handle;
  *count = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_read (grub_size_t argc __attribute__ ((unused)),
           const struct grub_bind_value *argv,
           struct grub_bind_value *out, grub_size_t *count)
{
  grub_tls_connection_t conn = grub_bind_handle_data (argv[0].value.handle,
                                                      "tls.connection");
  grub_uint64_t requested = argv[1].value.u64;
  grub_size_t length;
  unsigned char *buffer;
  grub_ssize_t n;
  if (!conn) return grub_errno;
  if (!requested || requested > 65536)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "TLS read size must be 1..65536");
  length = (grub_size_t) requested;
  buffer = grub_malloc (length);
  if (!buffer) return grub_errno;
  n = grub_tls_read (conn, buffer, length);
  if (n < 0)
    {
      grub_free (buffer);
      return grub_errno;
    }
  out[0].type = GRUB_BIND_BYTES;
  out[0].owned = 1;
  out[0].value.bytes.data = buffer;
  out[0].value.bytes.size = n;
  *count = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_write (grub_size_t argc __attribute__ ((unused)),
            const struct grub_bind_value *argv,
            struct grub_bind_value *out, grub_size_t *count)
{
  grub_tls_connection_t conn = grub_bind_handle_data (argv[0].value.handle,
                                                      "tls.connection");
  grub_ssize_t n;
  if (!conn) return grub_errno;
  n = grub_tls_write (conn, argv[1].value.bytes.data,
                      argv[1].value.bytes.size);
  if (n < 0) return grub_errno;
  out[0].type = GRUB_BIND_U64;
  out[0].value.u64 = n;
  *count = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_close (grub_size_t argc __attribute__ ((unused)),
            const struct grub_bind_value *argv,
            struct grub_bind_value *out __attribute__ ((unused)),
            grub_size_t *count)
{
  if (!grub_bind_handle_data (argv[0].value.handle, "tls.connection"))
    return grub_errno;
  *count = 0;
  return grub_bind_handle_close (argv[0].value.handle);
}

static const enum grub_bind_type tls_connect_args[] =
  { GRUB_BIND_STRING, GRUB_BIND_U64, GRUB_BIND_STRING };
static const enum grub_bind_type tls_read_args[] =
  { GRUB_BIND_HANDLE, GRUB_BIND_U64 };
static const enum grub_bind_type tls_write_args[] =
  { GRUB_BIND_HANDLE, GRUB_BIND_BYTES };
static const enum grub_bind_type tls_close_args[] = { GRUB_BIND_HANDLE };
static const struct grub_bind_export tls_exports[] =
{
  { "tls", "connect", tls_connect_args, 1, 3, bind_connect },
  { "tls", "read",    tls_read_args,    2, 2, bind_read },
  { "tls", "write",   tls_write_args,   2, 2, bind_write },
  { "tls", "close",   tls_close_args,   1, 1, bind_close }
};

GRUB_MOD_INIT (tls)
{
  tls_module = mod;
  tls_check_command = grub_register_command ("tls_check", tls_check,
                          "HOST [PORT [CA_FILE]]",
                          "Connect and verify a TLS server certificate.");
  if (grub_bind_register (mod, tls_exports,
                          sizeof (tls_exports) / sizeof (tls_exports[0])))
    grub_print_error ();
}

GRUB_MOD_FINI (tls)
{
  grub_bind_unregister (tls_module);
  grub_unregister_command (tls_check_command);
  /* Live connections pin the module, so the shared DRBG is no longer in use. */
  reset_random_state ();
  tls_module = NULL;
}
