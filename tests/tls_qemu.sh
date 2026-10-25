#!/bin/sh
# GRUB TLS certificate, native handshake and generic Lua Binding regression.
# Local TLS server + disposable QEMU disk images; no external network.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
platform=$(printenv GRUB_PLATFORM || echo x86_64-efi)
build=$(printenv GRUB_BUILD || echo "$repo/build-$platform")
ovmf_code=$(printenv OVMF_CODE || echo /usr/share/edk2/x64/OVMF_CODE.4m.fd)
ovmf_vars=$(printenv OVMF_VARS || echo /usr/share/edk2/x64/OVMF_VARS.4m.fd)

for tool in python3 openssl qemu-system-x86_64 timeout mkfs.fat mmd mcopy truncate; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
work=$(mktemp -d /tmp/grub-tls.XXXXXX)
passed=0
server_pid=
http_plain_pid=
http_tls_pid=
bad_server_pid=
rsa_server_pid=
expired_server_pid=
cleanup() {
  if test -n "$http_plain_pid"; then
    kill "$http_plain_pid" 2>/dev/null || true
    wait "$http_plain_pid" 2>/dev/null || true
  fi
  if test -n "$http_tls_pid"; then
    kill "$http_tls_pid" 2>/dev/null || true
    wait "$http_tls_pid" 2>/dev/null || true
  fi
  if test -n "$rsa_server_pid"; then
    kill "$rsa_server_pid" 2>/dev/null || true
    wait "$rsa_server_pid" 2>/dev/null || true
  fi
  if test -n "$expired_server_pid"; then
    kill "$expired_server_pid" 2>/dev/null || true
    wait "$expired_server_pid" 2>/dev/null || true
  fi
  if test -n "$bad_server_pid"; then
    kill "$bad_server_pid" 2>/dev/null || true
    wait "$bad_server_pid" 2>/dev/null || true
  fi
  if test -n "$server_pid"; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if test "$passed" = 1 && test "$(printenv TLS_KEEP || echo 0)" != 1; then
    rm -rf "$work"
  else
    echo "TLS evidence: $work" >&2
  fi
}
trap cleanup EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }

# Create one trusted private CA, an ECDSA/SHA-384 server certificate,
# and an unrelated CA that must be rejected at handshake.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout "$work/ca.key" -out "$work/ca.pem" -days 2 -nodes \
  -sha384 -subj "/CN=GRUB TLS Test Root" >"$work/openssl.log" 2>&1
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout "$work/server.key" -out "$work/server.csr" -nodes -sha384 \
  -subj "/CN=10.0.2.2" >>"$work/openssl.log" 2>&1
printf 'subjectAltName=IP:10.0.2.2,DNS:10.0.2.2\nextendedKeyUsage=serverAuth\n' >"$work/extensions.cnf"
openssl x509 -req -in "$work/server.csr" -CA "$work/ca.pem" \
  -CAkey "$work/ca.key" -CAcreateserial -out "$work/server.pem" \
  -days 2 -sha384 -extfile "$work/extensions.cnf" >>"$work/openssl.log" 2>&1
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout "$work/other.key" -out "$work/other-ca.pem" -days 2 -nodes \
  -sha384 -subj "/CN=Untrusted TLS Test Root" >>"$work/openssl.log" 2>&1

# The trusted CA also signs a certificate for a different hostname.
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -keyout "$work/wrong.key" -out "$work/wrong.csr" -nodes -sha384 -subj "/CN=wrong.example" >>"$work/openssl.log" 2>&1
printf 'subjectAltName=DNS:wrong.example\nextendedKeyUsage=serverAuth\n' >"$work/wrong.cnf"
openssl x509 -req -in "$work/wrong.csr" -CA "$work/ca.pem" -CAkey "$work/ca.key" -CAcreateserial -out "$work/wrong.pem" -days 2 -sha384 -extfile "$work/wrong.cnf" >>"$work/openssl.log" 2>&1

# Independently verify ECDHE-RSA with a server certificate from the
# trusted CA. Zero-day cert expires at issuance and must be rejected.
openssl req -new -newkey rsa:2048 -nodes -keyout "$work/rsa.key"   -out "$work/rsa.csr" -subj "/CN=10.0.2.2" >>"$work/openssl.log" 2>&1
openssl x509 -req -in "$work/rsa.csr" -CA "$work/ca.pem" -CAkey "$work/ca.key"   -CAcreateserial -out "$work/rsa.pem" -days 2 -sha256   -extfile "$work/extensions.cnf" >>"$work/openssl.log" 2>&1
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes   -keyout "$work/expired.key" -out "$work/expired.csr"   -subj "/CN=10.0.2.2" >>"$work/openssl.log" 2>&1
openssl x509 -req -in "$work/expired.csr" -CA "$work/ca.pem"   -CAkey "$work/ca.key" -CAcreateserial -out "$work/expired.pem"   -days 0 -sha384 -extfile "$work/extensions.cnf" >>"$work/openssl.log" 2>&1

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
http_plain_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
http_tls_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
rsa_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
expired_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
wrong_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
cat >"$work/test.lua" <<'LUA'
assert(type(tls.connect) == "function")
assert(type(tls.read) == "function")
assert(type(tls.write) == "function")
local port = tonumber(grub.getenv("tls_port"))
local ca = grub.getenv("grub_tls_ca")
assert(port ~= nil and ca ~= nil)
local connection = tls.connect("10.0.2.2", port, ca)
assert(type(connection) == "userdata")
local request = "GET /tls-test HTTP/1.1\r\nHost: 10.0.2.2\r\nConnection: close\r\n\r\n"
assert(tls.write(connection, request) == #request)
assert(not pcall(tls.read, connection, "4294967297"),
       "64-bit read length was truncated on i386")
local received = ""
while true do
  local fragment = tls.read(connection, 2048)
  if #fragment == 0 then break end
  received = received .. fragment
  assert(#received < 8192)
  if received:find("verified GRUB TLS transport\n", 1, true) then break end
end
assert(received:find("HTTP/1.1 200 OK", 1, true))
assert(received:find("verified GRUB TLS transport\n", 1, true))
assert(not pcall(tls.read, connection, 0))
tls.close(connection)
assert(not pcall(tls.write, connection, "x"))
collectgarbage("collect")
LUA
cat >"$work/hold.lua" <<'LUA'
local port = tonumber(grub.getenv("tls_port"))
tls_saved_connect = tls.connect
tls_held = tls.connect("10.0.2.2", port, grub.getenv("grub_tls_ca"))
assert(type(tls_held) == "userdata")
-- The test peer closes raw TCP after an unanswered request. This must not
-- be returned as successful TLS EOF; no authenticated close_notify arrived.
assert(not pcall(tls.read, tls_held, 1),
       "TCP FIN was misreported as authenticated TLS EOF")
assert(not pcall(tls.write, tls_held, "late"),
       "truncated TLS connection remained writable")
LUA
cat >"$work/clean.lua" <<'LUA'
local port = tonumber(grub.getenv("tls_port"))
local c = tls.connect("10.0.2.2", port, grub.getenv("grub_tls_ca"))
local request = "GET /tls-clean HTTP/1.1\r\nHost: 10.0.2.2\r\nConnection: close\r\n\r\n"
assert(tls.write(c, request) == #request)
local data = ""
while true do
  local fragment = tls.read(c, 2048)
  if #fragment == 0 then break end
  data = data .. fragment
  assert(#data < 8192)
end
assert(data:find("verified GRUB TLS transport", 1, true))
assert(not pcall(tls.write, c, "late"),
       "TLS connection accepted application data after close_notify")
tls.close(c)
LUA
cat >"$work/release.lua" <<'LUA'
tls.close(tls_held)
assert(not pcall(tls.read, tls_held, 1))
tls_held = nil
collectgarbage("collect")
LUA
cat >"$work/reload.lua" <<'LUA'
assert(type(tls.connect) == "function")
assert(not pcall(tls_saved_connect, "10.0.2.2"))
local h = tls.connect("10.0.2.2", tonumber(grub.getenv("tls_port")),
                      grub.getenv("grub_tls_ca"))
tls.close(h)
LUA
for control in C D; do
  cat >"$work/cancel-$control.lua" <<'LUA'
local c = tls.connect("10.0.2.2", tonumber(grub.getenv("tls_port")),
                      grub.getenv("grub_tls_ca"))
local request = "GET /tls-stall HTTP/1.1\r\nHost: 10.0.2.2\r\nConnection: close\r\n\r\n"
assert(tls.write(c, request) == #request)
print("TLS_CANCEL_MARK_BEGIN")
local ok, failure = pcall(tls.read, c, 1)
print("TLS_CANCEL_RESULT", tostring(ok), tostring(failure))
assert(not ok and tostring(failure):find("interrupted"),
       "TLS read did not honor requested cancel key")
assert(not pcall(tls.write, c, "late"),
       "cancelled TLS session accepted another write")
tls.close(c)
LUA
  sed -i "s/MARK/$control/g" "$work/cancel-$control.lua"
done
cat >"$work/http-test.lua" <<'LUA'
local http_port = grub.getenv("http_plain_port")
local https_port = grub.getenv("http_tls_port")
assert(type(http.request) == "function")
assert(type(https.request) == "function")
local r = http.request({url="http://10.0.2.2:"..http_port.."/file"})
assert(r.status == 200 and r.body == "GRUB_HTTP_FILE_CONTENT")
local r = https.request({
  url="https://10.0.2.2:"..https_port.."/echo",
  method="POST",
  headers={["X-HTTP-Test"]="nested-header", ["Content-Type"]="application/json"},
  body="hi-\0-binary"
})
assert(r.status == 200 and r.body == "hi-\0-binary")
assert(r.headers:find("X-Echo-Header: nested-header",1,true))
local h = https.request_stream({url="https://10.0.2.2:"..https_port.."/chunked"})
assert(https.status(h).status == 200)
local data = ""
repeat
  local part = https.read(h, 3)
  data = data .. part
  if #part == 0 then break end
until #data > 100
assert(data == "Wikipedia")
https.close(h)
assert(not pcall(https.read, h, 1))
local large = http.request({url="http://10.0.2.2:"..http_port.."/large"})
assert(large.status == 200 and #large.body == 131072)
assert(large.body:sub(1,16) == "0123456789abcdef")
local nf = https.request({url="https://10.0.2.2:"..https_port.."/missing"})
assert(nf.status == 404)
assert(not pcall(https.request, {url="http://10.0.2.2:"..http_port.."/file"}))
assert(not pcall(http.request, {url="http://10.0.2.2:"..http_port..
  "/file\r\nInjected: bad"}))
assert(not pcall(https.request, {url="https://10.0.2.2:"..https_port..
  "/echo", headers={["X-Evil"]="a\r\nHost: bad"}}))
http_saved_request = http.request
LUA
cat >"$work/http-vfs.lua" <<'LUA'
local plaintext = grub.getenv("http_plain_port")
local tlsport = grub.getenv("http_tls_port")
local function file_read(path, expected)
  local f = grub.file_open(path)
  assert(f, "VFS failed: "..path.." / "..tostring(grub_errmsg))
  local result = grub.file_read(f, #expected + 16)
  grub.file_close(f)
  if result ~= expected then
    print("HTTP_VFS_DEBUG", path, #result,
          string.format("%q", result))
  end
  assert(result == expected, path .. " : " .. tostring(#result))
end
file_read("(https,10.0.2.2:"..tlsport..")/file", "GRUB_HTTP_FILE_CONTENT")
file_read("(http,10.0.2.2:"..plaintext..")/file", "GRUB_HTTP_FILE_CONTENT")
local old_default_server = grub.getenv("net_default_server")
grub.setenv("net_default_server", "10.0.2.2:"..plaintext)
file_read("(http,)/file", "GRUB_HTTP_FILE_CONTENT")
grub.setenv("net_default_server", "10.0.2.2:"..tlsport)
file_read("(https,)/file", "GRUB_HTTP_FILE_CONTENT")
grub.setenv("net_default_server", old_default_server or "")
file_read("(https,10.0.2.2:"..tlsport..")/chunked", "Wikipedia")
local f = grub.file_open("(https,10.0.2.2:"..tlsport..")/large")
assert(f, tostring(grub_errmsg))
local start = grub.file_read(f, 32768)
assert(#start == 32768 and start:sub(1,16) == "0123456789abcdef")
grub.file_seek(f, 131056)
assert(grub.file_read(f, 16) == "0123456789abcdef")
grub.file_close(f)
LUA
cat >"$work/http-reload.lua" <<'LUA'
assert(not pcall(http_saved_request, {url="http://example.invalid/"}))
local port = grub.getenv("http_plain_port")
local r = http.request({url="http://10.0.2.2:"..port.."/file"})
assert(r.status == 200 and r.body == "GRUB_HTTP_FILE_CONTENT")
LUA
cat >"$work/cfg.in" <<'CFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo TLS_BEGIN
insmod net
insmod random
insmod tls
insmod lua
search --file --set=tls_root /ca.pem
set grub_tls_ca=($tls_root)/ca.pem
set tls_port=@PORT@
if net_dhcp; then echo TLS_DHCP_OK; else echo TLS_DHCP_FAIL; fi
if tls_check 10.0.2.2 @PORT@ ($tls_root)/other-ca.pem; then
  echo TLS_UNTRUSTED_ACCEPTED
else
  echo TLS_UNTRUSTED_REJECTED
fi
if tls_check 10.0.2.2 @RSAPORT@; then
  echo TLS_RSA_OK
else
  echo TLS_RSA_FAIL
fi
if tls_check 10.0.2.2 @EXPIREDPORT@; then
  echo TLS_EXPIRED_ACCEPTED
else
  echo TLS_EXPIRED_REJECTED
fi
if tls_check 10.0.2.2 @WRONGPORT@; then
  echo TLS_HOSTNAME_ACCEPTED
else
  echo TLS_HOSTNAME_REJECTED
fi
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_NATIVE_OK
else
  echo TLS_NATIVE_FAIL
fi
# A clock offset of -24h must make the freshly-issued certificate FUTURE.
set grub_tls_utc_offset=-1440
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_TIME_OFFSET_BROKEN
else
  echo TLS_TIME_OFFSET_OK
fi
unset grub_tls_utc_offset
# Stress closing the same native TLS transport repeatedly. No TCP-owned
# sockets or retransmission buffers may survive their owner's release.
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_REPEAT_1_OK
else
  echo TLS_REPEAT_1_FAIL
fi
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_REPEAT_2_OK
else
  echo TLS_REPEAT_2_FAIL
fi
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_REPEAT_3_OK
else
  echo TLS_REPEAT_3_FAIL
fi
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_REPEAT_4_OK
else
  echo TLS_REPEAT_4_FAIL
fi
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_REPEAT_5_OK
else
  echo TLS_REPEAT_5_FAIL
fi
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_REPEAT_6_OK
else
  echo TLS_REPEAT_6_FAIL
fi
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_REPEAT_7_OK
else
  echo TLS_REPEAT_7_FAIL
fi
if tls_check 10.0.2.2 @PORT@; then
  echo TLS_REPEAT_8_OK
else
  echo TLS_REPEAT_8_FAIL
fi
if lua ($tls_root)/test.lua; then
  echo TLS_LUA_OK
else
  echo TLS_LUA_FAIL
fi
if lua ($tls_root)/clean.lua; then
  echo TLS_CLEAN_EOF_OK
else
  echo TLS_CLEAN_EOF_FAIL
fi
if lua ($tls_root)/hold.lua; then
  echo TLS_HOLD_OK
else
  echo TLS_HOLD_FAIL
fi
if rmmod tls; then
  echo TLS_UNLOAD_WHILE_OPEN_BROKEN
else
  echo TLS_HANDLE_PIN_OK
fi
if lua ($tls_root)/release.lua; then
  echo TLS_RELEASE_OK
else
  echo TLS_RELEASE_FAIL
fi
if rmmod tls; then
  echo TLS_UNLOAD_OK
else
  echo TLS_UNLOAD_FAIL
fi
if insmod tls; then
  echo TLS_RELOAD_OK
else
  echo TLS_RELOAD_FAIL
fi
if lua ($tls_root)/reload.lua; then
  echo TLS_FRESH_OK
else
  echo TLS_FRESH_FAIL
fi
if lua ($tls_root)/cancel-C.lua; then
  echo TLS_CANCEL_C_OK
else
  echo TLS_CANCEL_C_FAIL
fi
if lua ($tls_root)/cancel-D.lua; then
  echo TLS_CANCEL_D_OK
else
  echo TLS_CANCEL_D_FAIL
fi
insmod http
set http_plain_port=@HTTPPORT@
set http_tls_port=@HTTPSPORT@
if lua ($tls_root)/http-test.lua; then
  echo HTTP_BINDING_OK
else
  echo HTTP_BINDING_FAIL
fi
if lua ($tls_root)/http-vfs.lua; then
  echo HTTP_VFS_OK
else
  echo HTTP_VFS_FAIL
fi
insmod configfile
source (https,10.0.2.2:@HTTPSPORT@)/boot.cfg
if rmmod http; then
  echo HTTP_UNLOAD_OK
else
  echo HTTP_UNLOAD_FAIL
fi
insmod http
if lua ($tls_root)/http-reload.lua; then
  echo HTTP_RELOAD_OK
else
  echo HTTP_RELOAD_FAIL
fi
echo TLS_DONE
halt
CFG
sed -e "s/@PORT@/$port/g" -e "s/@WRONGPORT@/$wrong_port/g"   -e "s/@RSAPORT@/$rsa_port/g" -e "s/@EXPIREDPORT@/$expired_port/g"   -e "s/@HTTPPORT@/$http_plain_port/g" -e "s/@HTTPSPORT@/$http_tls_port/g" "$work/cfg.in" >"$work/grub.cfg"
truncate -s 32M "$work/esp.img"
mkfs.fat -F 16 -n TLSTEST "$work/esp.img" >"$work/fs.log" 2>&1
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$work/ca.pem" ::/ca.pem
mcopy -i "$work/esp.img" "$work/other-ca.pem" ::/other-ca.pem
mcopy -i "$work/esp.img" "$work/test.lua" ::/test.lua
mcopy -i "$work/esp.img" "$work/http-test.lua" ::/http-test.lua
mcopy -i "$work/esp.img" "$work/http-vfs.lua" ::/http-vfs.lua
mcopy -i "$work/esp.img" "$work/http-reload.lua" ::/http-reload.lua
for script in clean hold release reload cancel-C cancel-D; do
  mcopy -i "$work/esp.img" "$work/$script.lua" "::/$script.lua"
done

case "$platform" in
  x86_64-efi)
    # UEFI firmware SerialIo does not always surface typed characters.
    # Activate the EFI console keyboard alongside serial logging, then
    # inject Ctrl-C/D via QEMU's virtual keyboard (QMP).
    sed -e 's/^terminal_input serial$/terminal_input serial console/' \
        -e '/^insmod net$/a insmod efinet' "$work/grub.cfg" >"$work/efi.cfg"
    mv "$work/efi.cfg" "$work/grub.cfg"
    mods='serial terminal normal echo test halt fat part_msdos part_gpt search search_fs_file net efinet random tls http lua minicmd configfile'
    "$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
      -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
      --install-modules="$mods" \
      --modules='serial terminal normal echo test halt fat part_msdos search' \
      "boot/grub/grub.cfg=$work/grub.cfg" >"$work/package.log" 2>&1
    mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
    cp "$ovmf_vars" "$work/vars.fd"
    qmp_path="$work/qmp.sock"
    set -- -qmp "unix:$qmp_path,server=on,wait=off" \
           -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
           -drive "if=pflash,format=raw,file=$work/vars.fd"
    ;;
  i386-pc)
    qmp_path=""
    mkdir -p "$work/iso/boot/grub"
    cp "$work/ca.pem" "$work/other-ca.pem" "$work/test.lua" \
       "$work/clean.lua" "$work/hold.lua" "$work/release.lua" \
       "$work/reload.lua" "$work/cancel-C.lua" "$work/cancel-D.lua" \
      "$work/http-test.lua" "$work/http-vfs.lua" "$work/http-reload.lua" "$work/iso/"
    sed '/^insmod net$/a insmod virtionet' "$work/grub.cfg" \
      >"$work/iso/boot/grub/grub.cfg"
    "$build/grub-mkrescue" -d "$build/grub-core" \
      -o "$work/boot.iso" "$work/iso" >"$work/package.log" 2>&1
    set -- -cdrom "$work/boot.iso" -boot d
    ;;
  *) fail "Unsupported platform $platform" ;;
esac

python3 "$repo/tests/tls_server.py" "$port" "$work/server.pem" \
  "$work/server.key" >"$work/server.log" 2>&1 &
server_pid=$!
for n in 1 2 3 4 5 6 7 8 9 10; do
  grep -q TLS_SERVER_READY "$work/server.log" && break
  sleep 0.1
done
grep -q TLS_SERVER_READY "$work/server.log" || fail "TLS test server did not start"

python3 "$repo/tests/tls_server.py" "$wrong_port" "$work/wrong.pem" "$work/wrong.key" >"$work/wrong-server.log" 2>&1 &
bad_server_pid=$!
for n in 1 2 3 4 5 6 7 8 9 10; do
  grep -q TLS_SERVER_READY "$work/wrong-server.log" && break
  sleep 0.1
done
grep -q TLS_SERVER_READY "$work/wrong-server.log" || fail "wrong-hostname TLS server did not start"

python3 "$repo/tests/tls_server.py" "$rsa_port" "$work/rsa.pem"   "$work/rsa.key" >"$work/rsa-server.log" 2>&1 &
rsa_server_pid=$!
python3 "$repo/tests/tls_server.py" "$expired_port" "$work/expired.pem"   "$work/expired.key" >"$work/expired-server.log" 2>&1 &
expired_server_pid=$!
for n in 1 2 3 4 5 6 7 8 9 10; do
  if grep -q TLS_SERVER_READY "$work/rsa-server.log" &&
     grep -q TLS_SERVER_READY "$work/expired-server.log"; then
    break
  fi
  sleep 0.1
done
grep -q TLS_SERVER_READY "$work/rsa-server.log" || fail "RSA TLS peer did not start"
grep -q TLS_SERVER_READY "$work/expired-server.log" || fail "expired TLS peer did not start"

python3 "$repo/tests/http_test_server.py" "$http_plain_port" >"$work/http-plain.log" 2>&1 &
http_plain_pid=$!
python3 "$repo/tests/http_test_server.py" "$http_tls_port" \
  --cert "$work/server.pem" --key "$work/server.key" >"$work/http-tls.log" 2>&1 &
http_tls_pid=$!
for n in 1 2 3 4 5 6 7 8 9 10; do
  if grep -q HTTP_SERVER_READY "$work/http-plain.log" &&
     grep -q HTTP_SERVER_READY "$work/http-tls.log"; then break; fi
  sleep 0.1
done
grep -q HTTP_SERVER_READY "$work/http-plain.log" || fail "HTTP test server unavailable"
grep -q HTTP_SERVER_READY "$work/http-tls.log" || fail "HTTPS test server unavailable"

# Slirp guest 10.0.2.2 reaches host loopback without root or tap networking.
if test "$platform" = i386-pc; then
  nic=virtio-net-pci
else
  nic=e1000
fi
python3 "${TLS_SERIAL_RUNNER:-$repo/tests/tls_serial_runner.py}"   --serial-log "$work/serial.log" --qmp "$qmp_path" \
  --timeout "$(printenv TLS_QEMU_TIMEOUT || echo 110)"   -- qemu-system-x86_64 \
  -machine q35,accel=tcg -cpu qemu64,+rdrand -m 512M \
  -display none -monitor none -no-reboot -serial stdio "$@" \
  -netdev user,id=tlsnet -device "$nic,netdev=tlsnet,mac=52:54:00:12:34:56" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  >"$work/qemu.log" 2>&1 || fail "QEMU failed"

for marker in TLS_DHCP_OK TLS_UNTRUSTED_REJECTED TLS_HOSTNAME_REJECTED TLS_RSA_OK TLS_EXPIRED_REJECTED TLS_NATIVE_OK TLS_TIME_OFFSET_OK TLS_REPEAT_1_OK TLS_REPEAT_2_OK TLS_REPEAT_3_OK TLS_REPEAT_4_OK TLS_REPEAT_5_OK TLS_REPEAT_6_OK TLS_REPEAT_7_OK TLS_REPEAT_8_OK TLS_LUA_OK TLS_CLEAN_EOF_OK TLS_HOLD_OK TLS_HANDLE_PIN_OK TLS_RELEASE_OK TLS_UNLOAD_OK TLS_RELOAD_OK TLS_FRESH_OK TLS_CANCEL_C_OK TLS_CANCEL_D_OK HTTP_BINDING_OK HTTP_VFS_OK HTTPS_CONFIG_EXECUTED HTTP_UNLOAD_OK HTTP_RELOAD_OK TLS_DONE; do
  grep -aq "$marker" "$work/serial.log" || { tail -n 60 "$work/serial.log"; fail "missing $marker"; }
done
! grep -aq TLS_UNTRUSTED_ACCEPTED "$work/serial.log" || fail "invalid CA accepted"
! grep -aq TLS_HOSTNAME_ACCEPTED "$work/serial.log" || fail "TLS hostname mismatch accepted"
! grep -aq TLS_EXPIRED_ACCEPTED "$work/serial.log" || fail "expired TLS certificate accepted"
grep -aq TLS_SERVER_HTTP_OK "$work/server.log" || fail "encrypted application payload not exchanged"
grep -aq TLS_SERVER_CLEAN_CLOSE_OK "$work/server.log" || fail "TLS close_notify exchange failed"
passed=1
echo "PASS: $platform TLS 1.2 CA/SAN/time, RSA, EOF, native/Binding, TCP cleanup, Ctrl-C/D"
