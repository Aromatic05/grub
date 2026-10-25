#!/usr/bin/env python3
"""A local TLS 1.2 test peer for tls_qemu.sh."""
import argparse
import socket
import ssl

parser = argparse.ArgumentParser()
parser.add_argument("port", type=int)
parser.add_argument("certificate")
parser.add_argument("key")
args = parser.parse_args()

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.maximum_version = ssl.TLSVersion.TLSv1_2
ctx.load_cert_chain(args.certificate, args.key)

with socket.socket() as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(8)
    listener.settimeout(150)
    print("TLS_SERVER_READY", args.port, flush=True)
    for _ in range(48):
        try:
            raw, addr = listener.accept()
        except socket.timeout:
            break
        try:
            with ctx.wrap_socket(raw, server_side=True) as client:
                client.settimeout(3)
                print("TLS_SERVER_HANDSHAKE", client.version(), flush=True)
                try:
                    request = client.recv(4096)
                except (TimeoutError, ssl.SSLError):
                    request = b""
                if request.startswith(b"GET /tls-stall HTTP/1.1\r\n"):
                    # Allow the guest's serial Ctrl-C/D to interrupt TLS read.
                    import time
                    print("TLS_SERVER_STALLING", flush=True)
                    time.sleep(4)
                elif request.startswith((b"GET /tls-test HTTP/1.1\r\n",
                                       b"GET /tls-clean HTTP/1.1\r\n")):
                    payload = b"verified GRUB TLS transport\n"
                    response = (b"HTTP/1.1 200 OK\r\nConnection: close\r\n"
                                + b"Content-Length: " + str(len(payload)).encode()
                                + b"\r\n\r\n" + payload)
                    client.sendall(response)
                    print("TLS_SERVER_HTTP_OK", flush=True)
                    if request.startswith(b"GET /tls-clean HTTP/1.1\r\n"):
                        # unwrap sends authenticated TLS close_notify, and
                        # waits for the client's close_notify response.
                        client.unwrap()
                        print("TLS_SERVER_CLEAN_CLOSE_OK", flush=True)
                else:
                    # No TLS close_notify: intentional truncation probe.
                    print("TLS_SERVER_PROBE", flush=True)
        except (OSError, ssl.SSLError) as exc:
            print("TLS_SERVER_PEER_REJECTED", type(exc).__name__, flush=True)
        finally:
            raw.close()
print("TLS_SERVER_STOPPED", flush=True)
