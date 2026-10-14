#!/bin/sh
# Isolated BIOS reset regression: verifies the BDA flag and QEMU reset event.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
pc_build=${A1C_PC_BUILD:-"$repo/build-i386-pc"}
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo 'Missing qemu-system-x86_64' >&2; exit 1; }
command -v xorriso >/dev/null 2>&1 || { echo 'Missing xorriso' >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo 'Missing python3' >&2; exit 1; }
test -x "$pc_build/grub-mkstandalone" || { echo "Missing $pc_build/grub-mkstandalone" >&2; exit 1; }
test -f "$repo/tests/a1ive_qmp_guest_event.py" || { echo "Missing QMP verifier" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-g12-bios-reset.XXXXXX")
passed=0
cleanup()
{
  if test "$passed" = 1 && test "${A1C_KEEP:-0}" != 1; then
    rm -rf "$work"
  else
    echo "Test output kept: $work" >&2
  fi
}
trap cleanup EXIT HUP INT TERM

fail()
{
  echo "FAIL: $*" >&2
  exit 1
}

reset_pc()
{
  name=$1
  args=$2
  expected=$3
  printf 'serial --speed=115200\nterminal_output serial\necho A1G12_RESET\nreset %s\necho A1G12_ERROR_RETURNED\n' \
    "$args" > "$work/$name.cfg"
  "$pc_build/grub-mkstandalone" -O i386-pc-eltorito -d "$pc_build/grub-core" \
    -o "$work/$name.eltorito.img" --fonts='' --locales='' --themes='' \
    --install-modules='serial terminal normal echo reboot' \
    --modules='serial terminal normal echo reboot' \
    "boot/grub/grub.cfg=$work/$name.cfg" > "$work/$name-pack.log" 2>&1 || {
      tail -n 8 "$work/$name-pack.log" >&2
      fail "$name: could not build BIOS image"
    }
  mkdir -p "$work/$name-iso/boot/grub"
  cp "$work/$name.eltorito.img" "$work/$name-iso/boot/grub/eltorito.img"
  xorriso -as mkisofs -quiet -R -b boot/grub/eltorito.img -no-emul-boot \
    -boot-load-size 4 -boot-info-table -o "$work/$name.iso" "$work/$name-iso" \
    > "$work/$name-iso.log" 2>&1 || fail "$name: could not make ISO"

  python3 -I "$repo/tests/a1ive_qmp_guest_event.py" \
    --expect guest-reset --timeout 35 \
    --qmp-log "$work/$name.qmp.json" --qemu-log "$work/$name.qemu" \
    --bda-output "$work/$name.bda" -- \
    qemu-system-x86_64 -machine pc,accel=tcg -m 256M -display none \
    -action reboot=shutdown,shutdown=pause \
    -serial "file:$work/$name.serial" -cdrom "$work/$name.iso" -boot d \
    || fail "$name: no QMP guest reset before host cleanup"
  grep -aq A1G12_RESET "$work/$name.serial" || fail "$name: reset command was not reached"
  ! grep -aq A1G12_ERROR_RETURNED "$work/$name.serial" || fail "$name: reset returned to GRUB"
  grep -aqE "0*472: 0x$expected" "$work/$name.bda" || {
    cat "$work/$name.bda" >&2
    fail "$name: BDA 0x472 did not equal 0x$expected"
  }
}

reset_pc cold -c 0000
reset_pc warm -w 1234
passed=1
echo 'PASS: BIOS reset -c / -w (BDA flag and QEMU reset event)'
