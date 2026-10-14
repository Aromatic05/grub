#!/bin/sh
# Strict EFI ResetSystem behavior probe for QEMU/OVMF.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
efi_build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for tool in qemu-system-x86_64 timeout mkfs.vfat mcopy mmd python3 dd mktemp; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing: $tool" >&2; exit 1; }
done
for path in "$efi_build/grub-mkstandalone" "$ovmf_code" "$ovmf_vars"; do
  test -f "$path" || { echo "Missing: $path" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/g12-efi-reset.XXXXXX")
echo "Artifacts: $work"
failed=0
passed=0
cleanup() {
  if test "$passed" = 1 && test "${A1C_KEEP:-0}" != 1; then
    rm -rf "$work"
  else
    echo "Test output kept: $work" >&2
  fi
}
trap cleanup EXIT HUP INT TERM

case_reset() {
  name=$1 arg=$2
  printf 'serial --speed=115200\nterminal_output serial\necho A1C_BEFORE_%s\nreset %s\necho A1C_RETURNED_%s\n' \
    "$name" "$arg" "$name" > "$work/$name.cfg"
  cp "$ovmf_vars" "$work/$name.vars"
  "$efi_build/grub-mkstandalone" -O x86_64-efi -d "$efi_build/grub-core" \
    -o "$work/$name.efi" --fonts='' --locales='' --themes='' \
    --install-modules='serial terminal normal echo reboot halt' \
    --modules='serial terminal normal echo reboot halt' \
    "boot/grub/grub.cfg=$work/$name.cfg" >"$work/$name.pack.log" 2>&1
  dd if=/dev/zero of="$work/$name.fat" bs=1M count=16 2>/dev/null
  mkfs.vfat "$work/$name.fat" >/dev/null
  mmd -i "$work/$name.fat" ::/EFI ::/EFI/BOOT
  mcopy -i "$work/$name.fat" "$work/$name.efi" ::/EFI/BOOT/BOOTX64.EFI
  { sleep 8; echo 'info status'; sleep 1; echo quit; } | \
    timeout 12s qemu-system-x86_64 -machine q35,accel=tcg -m 512M -display none \
    -no-reboot -action reboot=shutdown,shutdown=pause \
    -trace qemu_system_shutdown_request \
    -monitor stdio \
    -serial "file:$work/$name.serial" \
    -drive if=pflash,format=raw,unit=0,file="$ovmf_code",readonly=on \
    -drive if=pflash,format=raw,unit=1,file="$work/$name.vars" \
    -drive format=raw,file="$work/$name.fat" \
    >"$work/$name.qemu" 2>&1
  test -f "$work/$name.serial" || : > "$work/$name.serial"
  printf '\n=== %s (%s) ===\n' "$name" "$arg"
  printf 'serial: '; tr '\n' ' ' < "$work/$name.serial"; printf '\n'
  printf 'qemu trace: '; grep -E 'qemu_system_shutdown_request|reset|shutdown' "$work/$name.qemu" | tail -n 5 | tr '\n' ' '; printf '\n'
  printf 'monitor status: '; grep -aE 'VM status:|paused|running' "$work/$name.qemu" | tail -n 3 | tr '\n' ' '; printf '\n'
  if grep -aq "A1C_RETURNED_$name" "$work/$name.serial"; then
    echo "RESULT: firmware ResetSystem returned to GRUB"
    failed=1
  elif ! grep -aq "A1C_BEFORE_$name" "$work/$name.serial"; then
    echo "RESULT: GRUB reset command was not reached"
    failed=1
  else
    expected=2
    test "$arg" = -s && expected=6
    if grep -aq "qemu_system_shutdown_request reason=$expected" "$work/$name.qemu"; then
      echo "RESULT: QEMU observed request reason=$expected"
    else
      echo "RESULT: expected QEMU request reason=$expected was not observed"
      failed=1
    fi
  fi
}

case_reset cold -c
case_reset warm -w
case_reset force -f
case_reset shutdown -s
has_os_indications() {
  python3 -I - "$1" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
sys.exit(0 if "OsIndications".encode("utf-16-le") in data else 1)
PY
}
for name in cold warm shutdown; do
  if has_os_indications "$work/$name.vars"; then
    echo "FAIL: $name unexpectedly changed OsIndications"
    failed=1
  fi
done
if has_os_indications "$work/force.vars"; then
  echo 'PASS: reset -f persisted OsIndications in the isolated OVMF variable copy'
else
  echo 'FAIL: reset -f did not persist OsIndications in the isolated OVMF variable copy'
  failed=1
fi
test "$failed" = 0
passed=1
echo 'PASS: all four EFI reset requests reached QEMU without returning to GRUB'
