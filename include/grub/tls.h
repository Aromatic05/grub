/* Verified, stream-oriented TLS client for GRUB. GPL-3.0-or-later. */
#ifndef GRUB_TLS_H
#define GRUB_TLS_H 1

#include <grub/types.h>
#include <grub/err.h>

struct grub_tls_connection;
typedef struct grub_tls_connection *grub_tls_connection_t;

/* ca_path is a GRUB read-only file path to a PEM CA bundle. If NULL,
   the grub_tls_ca environment variable is used. Verification is mandatory:
   absent or invalid trust material always fails closed. Host is used both
   for DNS resolution and for TLS SNI/X.509 hostname verification.

   Each successful connection pins tls.mod until grub_tls_close. */
grub_tls_connection_t grub_tls_connect
  (const char *host, grub_uint16_t port, const char *ca_path);

/* The native transport does not depend on HTTP or any script runtime.
   Positive result: bytes transferred; 0: authenticated EOF (read only);
   -1: error in grub_errno. Writes complete the entire supplied buffer
   or return -1; they never report a successful partial write. */
grub_ssize_t grub_tls_read
  (grub_tls_connection_t connection, void *buf, grub_size_t length);
grub_ssize_t grub_tls_write
  (grub_tls_connection_t connection, const void *buf, grub_size_t length);
void grub_tls_close (grub_tls_connection_t connection);

#endif
