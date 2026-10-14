#!/bin/sh
# Real nested GRUB Multiboot v1 handoff via a BIOS GRUB ISO.
# QEMU does not need to interpret -kernel; outer GRUB supplies MBI registers.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
pc_build=${A1C_PC_BUILD:-"$repo/build-i386-pc"}
mb_build=${A1C_MULTIBOOT_BUILD:-"$repo/build-i386-multiboot"}
for tool in qemu-system-i386 timeout grub-file mktemp; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing: $tool" >&2; exit 1; }
done
test -x "$pc_build/grub-mkrescue" || { echo "Missing BIOS rescue build" >&2; exit 1; }
test -x "$mb_build/grub-mkstandalone" || { echo "Missing Multiboot standalone tool" >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-mb-chain.XXXXXX")
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

cat > "$work/guest.cfg" <<'GUEST'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo A1C_MB_GUEST_ENTERED
if insmod mbinfo; then echo A1C_OK_MBINFO_INSMOD; else echo A1C_ERROR_MBINFO_INSMOD; fi
if mbinfo; then echo A1C_OK_MBINFO_EXECUTED; else echo A1C_ERROR_MBINFO_EXECUTED; fi
echo A1C_MB_GUEST_DONE
halt
GUEST
"$mb_build/grub-mkstandalone" -O i386-multiboot -d "$mb_build/grub-core" \
  -o "$work/guest.elf" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules='serial terminal normal echo test halt mbinfo' \
  --modules='serial terminal normal echo test halt' \
  "boot/grub/grub.cfg=$work/guest.cfg" > "$work/guest-pack.log" 2>&1 || {
  tail -n 20 "$work/guest-pack.log" >&2
  fail "cannot package i386-multiboot GRUB guest"
}
grub-file --is-x86-multiboot "$work/guest.elf" || fail "guest image lacks valid Multiboot v1 header"
mkdir -p "$work/iso-root/boot/grub"
cp "$work/guest.elf" "$work/iso-root/boot/grub/g12-guest.elf"
cat > "$work/iso-root/boot/grub/grub.cfg" <<'HOST'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo A1C_MB_HOST_ENTERED
if insmod multiboot; then echo A1C_OK_MULTIBOOT_HOST; else echo A1C_ERROR_MULTIBOOT_HOST; fi
if multiboot (cd)/boot/grub/g12-guest.elf; then
  echo A1C_OK_MULTIBOOT_IMAGE
else
  echo A1C_ERROR_MULTIBOOT_IMAGE
fi
boot
echo A1C_ERROR_MULTIBOOT_RETURNED
halt
HOST
"$pc_build/grub-mkrescue" -d "$pc_build/grub-core" \
  -o "$work/outer.iso" "$work/iso-root" > "$work/outer-pack.log" 2>&1 || {
  tail -n 22 "$work/outer-pack.log" >&2
  fail "cannot package BIOS GRUB host ISO"
}
code=0
timeout 25s qemu-system-i386 -machine pc,accel=tcg -m 256 \
  -cdrom "$work/outer.iso" -boot d -display none -no-reboot \
  -serial "file:$work/serial.log" \
  -d int,guest_errors -D "$work/cpu.log" \
  > "$work/qemu.log" 2>&1 || code=$?
test "$code" = 0 || test "$code" = 124 || fail "QEMU process failed ($code)"
for marker in A1C_MB_HOST_ENTERED A1C_OK_MULTIBOOT_HOST \
  A1C_OK_MULTIBOOT_IMAGE A1C_MB_GUEST_ENTERED \
  A1C_OK_MBINFO_INSMOD A1C_OK_MBINFO_EXECUTED A1C_MB_GUEST_DONE; do
  grep -aq "$marker" "$work/serial.log" || fail "missing Multiboot marker $marker"
done
grep -aq 'flags: 0x' "$work/serial.log" || fail "mbinfo did not report MBI flags"
grep -aq 'mmap_length:' "$work/serial.log" || fail "mbinfo did not report copied memory map"
if grep -aq A1C_ERROR_ "$work/serial.log"; then
  grep -a A1C_ERROR_ "$work/serial.log" >&2
  fail "host/guest reported Multiboot error"
fi
passed=1
echo "PASS: BIOS GRUB chainloaded i386-multiboot GRUB and mbinfo reported live MBI"
