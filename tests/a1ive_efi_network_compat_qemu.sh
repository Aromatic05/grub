#!/bin/sh
# EFI QEMU regression for native DHCPv6 and legacy a1ive net_efi_* commands.
# All firmware variables and virtual network devices are disposable.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
efi_build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for c in qemu-system-x86_64 mkfs.vfat mmd mcopy truncate timeout; do
  command -v "$c" >/dev/null 2>&1 || { echo "Missing $c" >&2; exit 1; }
done
for f in "$efi_build/grub-mkstandalone" "$code" "$vars"; do
  test -f "$f" || { echo "Missing $f" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-efi-net.XXXXXX")
passed=0
cleanup() {
  if test "$passed" = 1 && test "${A1C_KEEP:-0}" != 1; then
    rm -rf "$work"
  else
    echo "Test output kept: $work" >&2
  fi
}
trap cleanup EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }

cat > "$work/grub.cfg" <<'CFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo A1C_EFINET_BEGIN
if insmod net; then echo A1C_OK_LOAD_NET; else echo A1C_ERROR_LOAD_NET; fi
if insmod efinet; then echo A1C_OK_LOAD_EFINET; else echo A1C_ERROR_LOAD_EFINET; fi
if insmod efi_netfs; then echo A1C_OK_LOAD_EFI_NETFS; else echo A1C_ERROR_LOAD_EFI_NETFS; fi

echo A1C_EFINET_CARDS_BEGIN
if net_efi_ls_cards; then echo A1C_OK_LIST_CARDS; else echo A1C_ERROR_LIST_CARDS; fi
echo A1C_EFINET_CARDS_END
if net_efi_ls_addr; then echo A1C_OK_LIST_ADDR; else echo A1C_ERROR_LIST_ADDR; fi
if net_efi_ls_routes; then echo A1C_OK_LIST_ROUTES; else echo A1C_ERROR_LIST_ROUTES; fi

# QEMU's actual e1000 card is named efinet0.  Require successful address
# assignment and verify that the address is visible in the live registry.
if net_efi_add_addr a1cv4 efinet0 192.0.2.99; then echo A1C_OK_ADD_IPV4; else echo A1C_ERROR_ADD_IPV4; fi
if net_efi_add_addr a1cv6 efinet0 2001:db8::123 52:54:00:ab:cd:ef; then echo A1C_OK_ADD_IPV6_MAC; else echo A1C_ERROR_ADD_IPV6_MAC; fi
echo A1C_EFINET_ADDED_ADDR_BEGIN
net_efi_ls_addr
echo A1C_EFINET_ADDED_ADDR_END

if net_efi_ls_cards extra; then echo A1C_ERROR_LIST_CARDS_ARG; else echo A1C_OK_LIST_CARDS_ARG; fi
if net_efi_ls_addr extra; then echo A1C_ERROR_LIST_ADDR_ARG; else echo A1C_OK_LIST_ADDR_ARG; fi
if net_efi_ls_routes extra; then echo A1C_ERROR_LIST_ROUTES_ARG; else echo A1C_OK_LIST_ROUTES_ARG; fi

if net_efi_add_addr bad missing-a1c-card 192.0.2.99; then echo A1C_ERROR_BAD_CARD_ADD; else echo A1C_OK_BAD_CARD_ADD; fi
if net_efi_add_addr bad missing-a1c-card 192.0.2.99 52:54:00:ab:cd:ef; then echo A1C_ERROR_BAD_CARD_MAC; else echo A1C_OK_BAD_CARD_MAC; fi
if net_efi_add_addr bad missing-a1c-card 192.0.2.99 INVALID-MAC; then echo A1C_ERROR_INVALID_MAC; else echo A1C_OK_INVALID_MAC; fi

if net_efi_bootp missing-a1c-card; then echo A1C_ERROR_BAD_BOOTPV4_CARD; else echo A1C_OK_BAD_BOOTPV4_CARD; fi
if net_efi_bootp6 missing-a1c-card; then echo A1C_ERROR_BAD_BOOTPV6_CARD; else echo A1C_OK_BAD_BOOTPV6_CARD; fi
if net_bootp6 missing-a1c-card; then echo A1C_ERROR_NATIVE_BOOTPV6_CARD; else echo A1C_OK_NATIVE_BOOTPV6_CARD; fi
if net_efi_bootp6 card1 card2; then echo A1C_ERROR_EXTRA_BOOTPV6_ARGS; else echo A1C_OK_EXTRA_BOOTPV6_ARGS; fi

# Exercise DHCPv4 transmission as a separate diagnostic.  A QEMU user-mode
# network may not send an OFFER even after receiving a valid DISCOVER.
# Do not count this handshake as a PASS unless a lease was obtained.
if net_efi_bootp efinet0; then echo A1C_DIAG_BOOTPV4_LEASE; else echo A1C_DIAG_BOOTPV4_NO_OFFER; fi

echo A1C_EFINET_DONE
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
  fail 'grub-mkstandalone failed to embed efi_netfs'
fi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$vars" "$work/vars.fd"
if ! timeout 75s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot \
  -serial "file:$work/serial.log" \
  -netdev user,id=a1cnet,ipv6=on \
  -device e1000,netdev=a1cnet,mac=52:54:00:ab:cd:ef \
  -drive "if=pflash,format=raw,readonly=on,file=$code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  > "$work/qemu.log" 2>&1; then
  fail 'QEMU EFI network guest did not halt cleanly'
fi
log="$work/serial.log"
for marker in A1C_EFINET_BEGIN A1C_EFINET_DONE \
  A1C_OK_LOAD_NET A1C_OK_LOAD_EFINET A1C_OK_LOAD_EFI_NETFS \
  A1C_OK_LIST_CARDS A1C_OK_LIST_ADDR A1C_OK_LIST_ROUTES \
  A1C_OK_ADD_IPV4 A1C_OK_ADD_IPV6_MAC \
  A1C_OK_LIST_CARDS_ARG A1C_OK_LIST_ADDR_ARG A1C_OK_LIST_ROUTES_ARG \
  A1C_OK_BAD_CARD_ADD A1C_OK_BAD_CARD_MAC A1C_OK_INVALID_MAC \
  A1C_OK_BAD_BOOTPV4_CARD A1C_OK_BAD_BOOTPV6_CARD A1C_OK_NATIVE_BOOTPV6_CARD \
  A1C_OK_EXTRA_BOOTPV6_ARGS; do
  grep -aq "$marker" "$log" || fail "missing GRUB marker: $marker"
done
if grep -aq A1C_ERROR_ "$log"; then
  grep -a A1C_ERROR_ "$log" >&2
  fail 'a net_efi or DHCPv6 behavior assertion reported failure'
fi
grep -aq 'a1cv4 .*192.0.2.99' "$log" || fail 'IPv4 address not visible after net_efi_add_addr'
grep -aq 'a1cv6 .*2001:db8' "$log" || fail 'IPv6 address not visible after net_efi_add_addr'
if grep -aq A1C_DIAG_BOOTPV4_NO_OFFER "$log"; then
  echo 'NOTE: DHCPv4 sent DISCOVER without an OFFER in this QEMU backend (lease not verified)'
fi
passed=1
echo 'PASS: EFI network legacy commands, real NIC IPv4/IPv6 assignment, DHCP argument/status checks'
