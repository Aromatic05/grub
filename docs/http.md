# Native HTTP/HTTPS in GRUB

`http.mod` registers both `http` and `https` as GRUB network file
protocols and publishes language-neutral HTTP client exports through
`grub_bind_register()`. There is no HTTP-specific Lua module.

One HTTP/1.1 parser and streaming body reader in
`grub-core/net/http_client.c` is shared by both network-file access and
script/native HTTP requests. The transport is GRUB TCP or verified
`tls.mod`. GRUB read-only VFS and WriteFS remain distinct.

## Network files

```grub
insmod net
insmod http
cat (http,example.com)/path/file.txt
cat (https,example.com)/path/file.txt
source (https,example.com)/grub.cfg
```

Custom ports work: `(https,example.com:8443)/file`.
`(http,)/path` and `(https,)/path` use `net_default_server`,
as do `(http)/path` and `(https)/path`. Without a default server,
specify the host.

For HTTPS, supply a trusted PEM CA bundle using
`set grub_tls_ca=(hd0,gpt1)/certificates/roots.pem`. Certificate
chain, hostname and UTC validity checks are mandatory.

Native files are read on demand. The decoder supports exact
`Content-Length`, `Transfer-Encoding: chunked` and close-delimited
responses without buffering the entire remote file. Seeks outside
the current buffer reopen using `Range: bytes=N-`; servers must return
a matching `206 Partial Content` and `Content-Range`. If the server
does not support Range, sequential reading still works. At chunked
EOF, the native file size is updated to avoid bufio padding.

## Lua and the language-independent Binding ABI

The existing Binding Registry exposes `http.*` and `https.*`
regardless of module load order. It supports a single nested string
map in a request record (currently at most eight custom headers).

```lua
local response = https.request({
  url = "https://example.com/api",
  method = "POST",
  headers = {
    ["Content-Type"] = "application/json",
    ["Authorization"] = "Bearer ..."
  },
  body = '{"message":"hello"}'
})
print(response.status, response.headers, response.body)
```

The response record contains a numeric `status`, raw HTTP `headers`
and a binary-safe `body`. Convenience `request` buffers at most 8 MiB.

For incremental responses, including SSE data:

```lua
local s = https.request_stream({
  url = "https://example.com/events",
  headers = { Accept = "text/event-stream" }
})
local meta = https.status(s)
while true do
  local chunk = https.read(s, 4096)
  if #chunk == 0 then break end
  -- Parse application frames across arbitrary chunk boundaries.
end
https.close(s)
```

Plain HTTP uses identical `http.request`, `http.request_stream`,
`http.read`, `http.status`, and `http.close` methods. Handles are
managed native resources, pinning their owner until close/GC.
Direct C streams independently pin http.mod for their entire lifetime,
so consumers cannot unload live native transport callbacks.
Unloaded bindings never revive old function references.

The C API in `include/grub/http.h` includes
`grub_http_stream_open`, `grub_http_stream_read`,
`grub_http_stream_close`, `grub_http_request` and
`grub_http_response_free`.

## Constraints and tests

- HTTP/1.1, one request per connection; no automatic redirects,
  HTTP/2, proxies, cookies or compressed bodies.
- GET, POST, PUT, PATCH, DELETE and HEAD are supported.
- Unsafe URL/control characters, ambiguous response framing, wrong
  Content-Range and malformed headers are rejected.
- TCP has bounded receive buffering and flow-control backpressure;
  HTTPS uses the existing TLS certificate and timeout policy.

Run the disposable real-network QEMU suite:

```sh
GRUB_PLATFORM=x86_64-efi ./tests/tls_qemu.sh
GRUB_PLATFORM=i386-pc ./tests/tls_qemu.sh
```

It exercises TLS identity, plain and encrypted GET, binary POST with
headers, chunked streaming, larger files, seek/Range, default-server
aliases, remote GRUB configuration loading and module unload/reload.
