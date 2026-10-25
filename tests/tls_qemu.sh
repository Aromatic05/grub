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
bad_server_pid=
cleanup() {
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

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
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
if lua ($tls_root)/test.lua; then
  echo TLS_LUA_OK
else
  echo TLS_LUA_FAIL
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
echo TLS_DONE
halt
CFG
sed -e "s/@PORT@/$port/g" -e "s/@WRONGPORT@/$wrong_port/g" "$work/cfg.in" >"$work/grub.cfg"
truncate -s 32M "$work/esp.img"
mkfs.fat -F 16 -n TLSTEST "$work/esp.img" >"$work/fs.log" 2>&1
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$work/ca.pem" ::/ca.pem
mcopy -i "$work/esp.img" "$work/other-ca.pem" ::/other-ca.pem
mcopy -i "$work/esp.img" "$work/test.lua" ::/test.lua
for script in hold release reload; do
  mcopy -i "$work/esp.img" "$work/$script.lua" "::/$script.lua"
done

case "$platform" in
  x86_64-efi)
    sed '/^insmod net$/a insmod efinet' "$work/grub.cfg" >"$work/efi.cfg"
    mv "$work/efi.cfg" "$work/grub.cfg"
    mods='serial terminal normal echo test halt fat part_msdos part_gpt search search_fs_file net efinet random tls lua minicmd'
    "$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
      -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
      --install-modules="$mods" \
      --modules='serial terminal normal echo test halt fat part_msdos search' \
      "boot/grub/grub.cfg=$work/grub.cfg" >"$work/package.log" 2>&1
    mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
    cp "$ovmf_vars" "$work/vars.fd"
    set -- -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
           -drive "if=pflash,format=raw,file=$work/vars.fd"
    ;;
  i386-pc)
    mkdir -p "$work/iso/boot/grub"
    cp "$work/ca.pem" "$work/other-ca.pem" "$work/test.lua"       "$work/hold.lua" "$work/release.lua" "$work/reload.lua" "$work/iso/"
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

# Slirp guest 10.0.2.2 reaches host loopback without root or tap networking.
if test "$platform" = i386-pc; then
  nic=virtio-net-pci
else
  nic=e1000
fi
timeout "$(printenv TLS_QEMU_TIMEOUT || echo 110s)" qemu-system-x86_64 \
  -machine q35,accel=tcg -cpu qemu64,+rdrand -m 512M \
  -display none -monitor none -no-reboot -serial "file:$work/serial.log" "$@" \
  -netdev user,id=tlsnet -device "$nic,netdev=tlsnet,mac=52:54:00:12:34:56" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  >"$work/qemu.log" 2>&1 || fail "QEMU failed"

for marker in TLS_DHCP_OK TLS_UNTRUSTED_REJECTED TLS_HOSTNAME_REJECTED TLS_NATIVE_OK TLS_LUA_OK TLS_HOLD_OK TLS_HANDLE_PIN_OK TLS_RELEASE_OK TLS_UNLOAD_OK TLS_RELOAD_OK TLS_FRESH_OK TLS_DONE; do
  grep -aq "$marker" "$work/serial.log" || { tail -n 60 "$work/serial.log"; fail "missing $marker"; }
done
! grep -aq TLS_UNTRUSTED_ACCEPTED "$work/serial.log" || fail "invalid CA accepted"
! grep -aq TLS_HOSTNAME_ACCEPTED "$work/serial.log" || fail "TLS hostname mismatch accepted"
grep -aq TLS_SERVER_HTTP_OK "$work/server.log" || fail "encrypted application payload not exchanged"
passed=1
echo "PASS: $platform verified TLS 1.2, rejected untrusted CA, native command and Bind ABI roundtrip"
