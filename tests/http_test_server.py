#!/usr/bin/env python3
"""One-request-per-connection HTTP/1.1 test peer, with optional real TLS."""
import argparse
import socket
import ssl
import sys
from urllib.parse import urlsplit

p = argparse.ArgumentParser()
p.add_argument("port", type=int)
p.add_argument("--cert")
p.add_argument("--key")
args = p.parse_args()
context = None
if args.cert:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(args.cert, args.key)

def service(sock):
    sock.settimeout(6)
    content = bytearray()
    while b"\r\n\r\n" not in content and len(content) < 16384:
        piece = sock.recv(4096)
        if not piece:
            return
        content.extend(piece)
    raw, _, tail = bytes(content).partition(b"\r\n\r\n")
    header_lines = raw.split(b"\r\n")
    method, target, version = header_lines[0].decode().split(" ")
    headers = {}
    for line in header_lines[1:]:
        name, sep, value = line.partition(b":")
        if sep:
            headers[name.decode().lower()] = value.strip().decode()
    length = int(headers.get("content-length", "0"))
    if length > 1024 * 1024:
        return
    body = bytearray(tail)
    while len(body) < length:
        part = sock.recv(min(4096, length - len(body)))
        if not part: return
        body.extend(part)
    path = urlsplit(target).path
    status = "200 OK"
    extra = b""
    if path == "/file":
        payload = b"GRUB_HTTP_FILE_CONTENT"
    elif path == "/boot.cfg":
        payload = b"echo HTTPS_CONFIG_EXECUTED\n"
    elif path == "/echo":
        payload = bytes(body)
        extra = b"X-Echo-Header: " + headers.get("x-http-test", "?").encode() + b"\r\n"
    elif path == "/large":
        payload = b"0123456789abcdef" * 8192
    elif path == "/chunked":
        sock.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n"
                     b"Transfer-Encoding: chunked\r\n\r\n"
                     b"4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n")
        print("HTTP_CHUNK_SENT", flush=True)
        return
    elif path == "/missing":
        status = "404 Not Found"
        payload = b"not found"
    else:
        payload = b"unknown path"
        status = "404 Not Found"
    start = 0
    if "range" in headers:
        val = headers["range"]
        if not val.startswith("bytes=") or not val.endswith("-"):
            return
        try:
            start = int(val[6:-1])
        except ValueError:
            return
        if start >= len(payload):
            status = "416 Range Not Satisfiable"
            payload = b""
        else:
            status = "206 Partial Content"
            extra += f"Content-Range: bytes {start}-{len(payload)-1}/{len(payload)}\r\n".encode()
            payload = payload[start:]
    response = (f"HTTP/1.1 {status}\r\nContent-Length: {len(payload)}\r\n".encode()
                + b"Connection: close\r\n" + extra + b"\r\n" + payload)
    sock.sendall(response)
    print("HTTP_SERVED", method, path, status, len(payload), flush=True)

with socket.socket() as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(12)
    listener.settimeout(180)
    print("HTTP_SERVER_READY", args.port, flush=True)
    for _ in range(48):
        try:
            raw, address = listener.accept()
        except socket.timeout:
            break
        try:
            if context:
                with context.wrap_socket(raw, server_side=True) as connection:
                    service(connection)
            else:
                with raw:
                    service(raw)
        except (OSError, ValueError, ssl.SSLError) as exc:
            print("HTTP_PEER_FAILED", str(exc), flush=True)
        finally:
            raw.close()
