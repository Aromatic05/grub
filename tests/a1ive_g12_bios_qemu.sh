#!/bin/sh
# BIOS/QEMU regression for the i386-pc early protected-mode transition.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${G12_BIOS_BUILD:-${A1C_PC_BUILD:-"$repo/build-i386-pc"}}
work=${G12_BIOS_WORKDIR:-$(mktemp -d "${TMPDIR:-/tmp}/a1ive-bios-boot.XXXXXX")}
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

for tool in qemu-system-i386 timeout grep; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing tool: $tool" >&2; exit 1; }
done
test -x "$build/grub-mkrescue" || { echo "Missing BIOS build: $build/grub-mkrescue" >&2; exit 1; }
test -d "$build/grub-core" || { echo "Missing BIOS modules: $build/grub-core" >&2; exit 1; }

mkdir -p "$work/iso-root/boot/grub"
cat > "$work/iso-root/boot/grub/grub.cfg" <<'CFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo G12_BIOS_REACHED
halt
CFG

"$build/grub-mkrescue" -d "$build/grub-core" \
  -o "$work/g12-bios.iso" "$work/iso-root" > "$work/mkrescue.log" 2>&1

status=0
timeout 12s qemu-system-i386 -machine pc -m 128 \
  -cdrom "$work/g12-bios.iso" -boot d -display none \
  -serial "file:$work/serial.log" -no-reboot -no-shutdown \
  -d int,in_asm,guest_errors -D "$work/qemu.log" \
  > "$work/qemu.stdout.log" 2>&1 || status=$?
test "$status" -eq 0 -o "$status" -eq 124 || {
  echo "QEMU failed with status $status" >&2
  exit 1
}
grep -aq 'G12_BIOS_REACHED' "$work/serial.log" || {
  echo "GRUB serial marker was not reached" >&2
  exit 1
}
if grep -aq 'Triple fault' "$work/qemu.log"; then
  echo "QEMU recorded a triple fault" >&2
  exit 1
fi

echo "PASS: BIOS GRUB reached G12_BIOS_REACHED ($work/serial.log)"
passed=1
