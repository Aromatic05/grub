# Standalone TLS transport in GRUB

The tls.mod module provides an independently loadable, verified TLS 1.2
client over GRUB TCP and DNS. It depends on net, datetime and random, not
Lua, HTTP or the GRUB Agent. Its public C interface is include/grub/tls.h.
Lua uses the existing, language-neutral Binding Registry rather than a
separate tls_lua or netlua module.

## Trust and security

- Certificate-chain and hostname validation are mandatory. The client
  sends SNI and checks validity using UTC. EFI's known TimeZone is
  converted with the UEFI rule UTC = local clock + TimeZone minutes;
  daylight adjustments are already reflected in that offset. On BIOS
  and firmware reporting an unspecified TimeZone, the RTC is assumed UTC.
  If the RTC is local, set grub_tls_utc_offset to the UTC-minus-RTC
  offset in minutes (-1440..1440) before connecting. An explicit value
  also overrides firmware TimeZone when it is incorrect.
- An explicit PEM trust bundle must be supplied via the grub_tls_ca
  environment variable or the connection's CA argument. The bundle is
  opened using the original read-only GRUB VFS. A missing, unreadable or
  invalid bundle fails closed; there is no insecure verification mode.
- EFI RNG (where available) or GRUB's hardware entropy interface is
  required. There is no time/address-based substitute for secure entropy.
  A securely seeded CTR-DRBG is shared across requests and obeys Mbed TLS
  reseed policy. Only initial entropy failures are retried, never failed
  certificate checks or application requests.
- Receive buffering is bounded and the handshake/idle deadline is 30s.
  Ctrl-C, Ctrl-D, ESC and F4 interrupt TCP SYN waits and
  TLS handshake/read/write waits. Other GRUB TCP clients retain their
  existing connection behavior.
- TCP FIN/RST is never a successful TLS EOF: a zero-length TLS read
  requires an authenticated close_notify alert. The local close API
  sends close_notify on established connections, then closes and disposes
  the TCP socket and its queued retransmits/packets. This is a bounded
  best-effort shutdown, not a blocking wait for a peer's acknowledgement.
  Open connections pin tls.mod against module unloading.
- The implementation uses freestanding Mbed TLS 3.6.7, vendored from the
  upstream GPL-compatible source distribution. See
  grub-core/lib/mbedtls/README.grub for attribution and licensing.

The transport supports TLS 1.2, not TLS 1.3. HTTPS URL handling and the
(https,host)/path native file protocol are separate, higher-level work.
The existing (http,host)/path protocol is unaffected.

## Native command

Configure a network card and a trusted PEM bundle, then run:

    insmod net
    insmod tls
    set grub_tls_ca=(hd0,gpt1)/roots.pem
    tls_check example.com 443

The native tls_check HOST [PORT [CA_FILE]] command verifies the server,
prints the negotiated TLS version and ciphersuite, then closes the
connection. It does not require Lua.

## Native C API

    grub_tls_connection_t connection =
      grub_tls_connect ("example.com", 443, "(hd0,gpt1)/roots.pem");
    if (!connection)
      return grub_errno;
    grub_ssize_t n = grub_tls_write (connection, request, length);
    grub_ssize_t count = n >= 0
      ? grub_tls_read (connection, response, sizeof (response)) : -1;
    grub_tls_close (connection);

The connect/read/write/close functions belong to tls.mod, not the Core.
Write completes the requested buffer or returns -1. A successful read
returns a length, zero indicates authenticated TLS close_notify (or a
zero-length read request), and -1 signals an error (including truncated
streams and interrupted waits). Fatal I/O errors invalidate the connection
for further transfers, even if the caller catches the error in Lua.
A caller that uses a known application-level length can close the TLS
connection after reading exactly that length without waiting for the peer
to signal TLS EOF.

## Script API via the common Binding Registry

    -- insmod tls and insmod lua may be executed in either order.
    local c = tls.connect("example.com", 443, "(hd0,gpt1)/roots.pem")
    local request = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n"
    assert(tls.write(c, request) == #request)
    local data = tls.read(c, 4096)
    tls.close(c)

Available methods are tls.connect(host [,port [,ca_path]]),
tls.read(handle, size), tls.write(handle, bytes), and tls.close(handle).
Handles are managed userdata: garbage collection closes forgotten
connections, stale handles fail, and an active connection prevents
rmmod tls. Old function references remain revoked after unload/reload.

HTTP request generation, redirects and SSE framing are intentionally
outside the transport module.

## Testing

    GRUB_PLATFORM=x86_64-efi ./tests/tls_qemu.sh
    GRUB_PLATFORM=i386-pc ./tests/tls_qemu.sh

The disposable tests run a local TLS 1.2 server and create short-lived
ECDSA and RSA certificates with a private CA. They check trusted,
untrusted, expired and wrong-hostname certificates, UTC offset handling,
exact 64-bit Lua read lengths, authenticated close_notify versus raw
TCP truncation, repeated native connections, encrypted data transfer,
interactive cancellation and module reference/unload behavior.
No tap network or external service is required.
