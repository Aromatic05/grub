#!/bin/sh
# Real i386-pc BIOS/SeaBIOS test for legacy a1ive modules.
# No host disk writes; ISO and QEMU serial output stay under TMPDIR.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${A1C_PC_BUILD:-"$repo/build-i386-pc"}
for tool in qemu-system-i386 timeout grep mktemp; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
test -x "$build/grub-mkrescue" || { echo "Missing grub-mkrescue in $build" >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-bios-legacy.XXXXXX")
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
mkdir -p "$work/iso-root/boot/grub"
cat > "$work/iso-root/boot/grub/grub.cfg" <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo A1C_G12_BIOS_BEGIN
if insmod biosdisk; then echo A1C_OK_BIOSDISK; else echo A1C_ERROR_BIOSDISK; fi
if insmod geometry; then echo A1C_OK_INSMOD_GEOMETRY; else echo A1C_ERROR_INSMOD_GEOMETRY; fi
if geometry -n (cd) a1c_drive; then
  if [ -n "$a1c_drive" ]; then echo A1C_OK_GEOMETRY_DRIVE; else echo A1C_ERROR_GEOMETRY_EMPTY; fi
else
  echo A1C_ERROR_GEOMETRY_DRIVE
fi
if geometry -h (cd) a1c_heads; then
  if [ -n "$a1c_heads" ]; then echo A1C_OK_GEOMETRY_HEADS; else echo A1C_ERROR_GEOMETRY_HEADS_EMPTY; fi
else
  echo A1C_ERROR_GEOMETRY_HEADS
fi
if insmod vboot; then echo A1C_OK_INSMOD_VBOOT; else echo A1C_ERROR_INSMOD_VBOOT; fi
if vboot16 (cd)/missing-g12-vboot-loader.bin; then
  echo A1C_ERROR_VBOOT16_MISSING
else
  echo A1C_OK_VBOOT16_MISSING_REJECTED
fi
if vbootinsmod missing-g12-vboot-elf-module; then
  echo A1C_ERROR_VBOOTINSMOD_MISSING
else
  echo A1C_OK_VBOOTINSMOD_MISSING_REJECTED
fi
set vbootloader=(cd)/missing-g12-loader
if vboot; then
  echo A1C_ERROR_VBOOT_MISSING
else
  echo A1C_OK_VBOOT_MISSING_REJECTED
fi
echo A1C_G12_BIOS_DONE
halt
GRUB
"$build/grub-mkrescue" -d "$build/grub-core" \
  -o "$work/bios-legacy.iso" "$work/iso-root" > "$work/mkrescue.log" 2>&1 || {
  tail -n 15 "$work/mkrescue.log" >&2
  fail "could not package BIOS ISO"
}
status=0
timeout 20s qemu-system-i386 -machine pc,accel=tcg -m 128 \
  -cdrom "$work/bios-legacy.iso" -boot d -display none -no-reboot \
  -serial "file:$work/serial.log" \
  > "$work/qemu.log" 2>&1 || status=$?
test "$status" = 0 || test "$status" = 124 || fail "QEMU failed exit=$status"
for marker in A1C_G12_BIOS_BEGIN A1C_G12_BIOS_DONE \
  A1C_OK_BIOSDISK A1C_OK_INSMOD_GEOMETRY A1C_OK_INSMOD_VBOOT \
  A1C_OK_GEOMETRY_DRIVE A1C_OK_GEOMETRY_HEADS \
  A1C_OK_VBOOT16_MISSING_REJECTED \
  A1C_OK_VBOOTINSMOD_MISSING_REJECTED A1C_OK_VBOOT_MISSING_REJECTED; do
  grep -aq "$marker" "$work/serial.log" || fail "missing BIOS marker $marker"
done
if grep -aq A1C_ERROR_ "$work/serial.log"; then
  grep -a A1C_ERROR_ "$work/serial.log" >&2
  fail "BIOS GRUB reported behavior failure"
fi
passed=1
echo "PASS: i386-pc BIOS loaded geometry/vboot and rejected invalid VBoot payloads"
