#!/bin/sh
# Positive DHCPv6 Solicit/Advertise/Request/Reply handshake through QEMU socket netdev.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
efi_build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
server="$repo/tests/a1ive_dhcpv6_socket_server.py"
for c in qemu-system-x86_64 mkfs.vfat mmd mcopy truncate timeout python3; do
  command -v "$c" >/dev/null 2>&1 || { echo "Missing $c" >&2; exit 1; }
done
for f in "$efi_build/grub-mkstandalone" "$code" "$vars" "$server"; do
  test -f "$f" || { echo "Missing $f" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-dhcp6.XXXXXX")
passed=0
cleanup() {
  test -z "${server_pid:-}" || kill "$server_pid" 2>/dev/null || true
  if test "$passed" = 1 && test "${A1C_KEEP:-0}" != 1; then rm -rf "$work"; else echo "Test output kept: $work" >&2; fi
}
trap cleanup EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }
port=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')

cat > "$work/grub.cfg" <<'CFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set debug=net,bootp
echo A1C_DHCP6_BEGIN
insmod net
insmod efinet
insmod efi_netfs
if net_efi_bootp6 efinet0; then echo A1C_DHCP6_OK; else echo A1C_DHCP6_ERROR; fi
net_efi_ls_addr
echo A1C_DHCP6_DONE
halt
CFG
truncate -s 48M "$work/esp.img"
mkfs.vfat -F 16 -n A1CNET "$work/esp.img" > "$work/mkfs.log" 2>&1
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mods='serial terminal normal echo test halt fat part_msdos net efinet efi_netfs'
preloaded='serial terminal normal echo test halt fat part_msdos'
if ! "$efi_build/grub-mkstandalone" -O x86_64-efi -d "$efi_build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules="$mods" --modules="$preloaded" \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1; then
  tail -n 15 "$work/pack.log" >&2
  fail 'grub-mkstandalone failed'
fi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$vars" "$work/vars.fd"
python3 "$server" "$port" > "$work/server.log" 2>&1 &
server_pid=$!
if ! timeout 90s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 -m 512M \
  -display none -no-reboot -serial "file:$work/serial.log" \
  -netdev "socket,id=a1cnet,listen=127.0.0.1:$port" \
  -object "filter-dump,id=a1cpcap,netdev=a1cnet,file=$work/dhcp6.pcap" \
  -device e1000,netdev=a1cnet,mac=52:54:00:ab:cd:ef \
  -drive "if=pflash,format=raw,readonly=on,file=$code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" > "$work/qemu.log" 2>&1; then
  fail 'QEMU EFI guest did not halt cleanly'
fi
wait "$server_pid" || { cat "$work/server.log" >&2; fail 'DHCPv6 server did not observe a complete exchange'; }
server_pid=
grep -aq 'SERVER_EXCHANGE SOLICIT -> REQUEST' "$work/server.log" || fail 'missing audited server exchange'
grep -aq A1C_DHCP6_OK "$work/serial.log" || fail 'GRUB DHCPv6 command failed'
grep -aEq '2001:db8:1::123|2001:db8:1:0:0:0:0:123' "$work/serial.log" || fail 'GRUB did not install the offered DHCPv6 address'
grep -aq A1C_DHCP6_DONE "$work/serial.log" || fail 'GRUB did not reach completion marker'
passed=1
echo 'PASS: GRUB completed DHCPv6 Solicit/Advertise/Request/Reply; server verified request and GRUB reported 2001:db8:1::123'
