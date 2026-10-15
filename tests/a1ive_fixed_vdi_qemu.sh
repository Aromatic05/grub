#!/bin/sh
# Real fixed/static VDI end-to-end test, including legacy 2 MiB offset.
# QEMU firmware, disks, and all generated images are private test fixtures.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for tool in python3 qemu-system-x86_64 qemu-img timeout mkfs.vfat mcopy mmd truncate; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing tool: $tool" >&2; exit 1; }
done
for file in "$build/grub-mkstandalone" "$code" "$vars"; do
  test -f "$file" || { echo "Missing: $file" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-fixed-vdi.XXXXXX")
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

truncate -s 16M "$work/raw.img"
mkfs.vfat -F 16 -n VDIDATA "$work/raw.img" >/dev/null
printf 'A1IVE_FIXED_VDI_OK\n' > "$work/MARKER.TXT"
mcopy -i "$work/raw.img" "$work/MARKER.TXT" ::/MARKER.TXT
qemu-img convert -f raw -O vdi -o static=on "$work/raw.img" "$work/fixed.vdi"

python3 - "$work/fixed.vdi" "$work/raw.img" "$work" <<'PY'
import pathlib
import sys
fixed = pathlib.Path(sys.argv[1]).read_bytes()
raw = pathlib.Path(sys.argv[2]).read_bytes()
work = pathlib.Path(sys.argv[3])
assert int.from_bytes(fixed[0x4c:0x50], "little") == 2
assert int.from_bytes(fixed[0x170:0x178], "little") == len(raw)
offset = int.from_bytes(fixed[0x158:0x15c], "little")
assert offset > 0 and fixed[offset + 510:offset + 512] == b"\x55\xaa"

# Historical a1ive images placed payload at 2 MiB.
legacy = bytearray(fixed[:0x180])
legacy[0x158:0x15c] = (2 * 1048576).to_bytes(4, "little")
(work / "legacy.vdi").write_bytes(legacy + bytes(2 * 1048576 - len(legacy)) + raw)

# Dynamic image and impossible declared offset must not yield mounted FAT data.
dynamic = bytearray(fixed)
dynamic[0x4c:0x50] = (1).to_bytes(4, "little")
(work / "dynamic.vdi").write_bytes(dynamic)
corrupt = bytearray(fixed)
corrupt[0x158:0x15c] = (0xfffffff0).to_bytes(4, "little")
(work / "corrupt.vdi").write_bytes(corrupt)
PY

cat > "$work/grub.cfg" <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set root=hd0
echo A1C_FIXED_VDI_BEGIN
if insmod fixed_vdi; then echo A1C_OK_VDI_MODULE; else echo A1C_ERROR_VDI_MODULE; fi
if insmod loopback; then echo A1C_OK_LOOPBACK_MODULE; else echo A1C_ERROR_LOOPBACK_MODULE; fi
if loopback valid (hd0)/fixed.vdi; then
  if cat (valid)/MARKER.TXT; then echo A1C_OK_VDI_NEW_OFFSET; else echo A1C_ERROR_VDI_NEW_READ; fi
  loopback -d valid
else
  echo A1C_ERROR_VDI_NEW_OPEN
fi
if loopback legacy (hd0)/legacy.vdi; then
  if cat (legacy)/MARKER.TXT; then echo A1C_OK_VDI_LEGACY_OFFSET; else echo A1C_ERROR_VDI_LEGACY_READ; fi
  loopback -d legacy
else
  echo A1C_ERROR_VDI_LEGACY_OPEN
fi
if loopback dynamic (hd0)/dynamic.vdi; then
  if cat (dynamic)/MARKER.TXT; then echo A1C_ERROR_VDI_DYNAMIC_ACCEPTED; else echo A1C_OK_VDI_DYNAMIC_REJECT; fi
  loopback -d dynamic
else
  echo A1C_OK_VDI_DYNAMIC_REJECT
fi
if loopback corrupt (hd0)/corrupt.vdi; then
  if cat (corrupt)/MARKER.TXT; then echo A1C_ERROR_VDI_CORRUPT_ACCEPTED; else echo A1C_OK_VDI_CORRUPT_REJECT; fi
  loopback -d corrupt
else
  echo A1C_OK_VDI_CORRUPT_REJECT
fi
echo A1C_FIXED_VDI_DONE
halt
GRUB

truncate -s 120M "$work/esp.img"
mkfs.vfat -F 16 -n VDIROOT "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
for name in fixed legacy dynamic corrupt; do
  mcopy -i "$work/esp.img" "$work/$name.vdi" "::$name.vdi"
done
mods='serial terminal normal echo test halt fat part_msdos cat loopback fixed_vdi vhd'
preload='serial terminal normal echo test halt fat part_msdos cat'
if ! "$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules="$mods" --modules="$preload" \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1; then
  tail -n 15 "$work/pack.log" >&2
  fail "cannot package VDI GRUB EFI"
fi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$vars" "$work/vars.fd"
if ! timeout 70s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  > "$work/qemu.log" 2>&1; then
  fail "QEMU did not exit cleanly"
fi
for marker in A1C_FIXED_VDI_BEGIN A1C_FIXED_VDI_DONE A1C_OK_VDI_MODULE \
  A1C_OK_LOOPBACK_MODULE A1C_OK_VDI_NEW_OFFSET A1C_OK_VDI_LEGACY_OFFSET \
  A1C_OK_VDI_DYNAMIC_REJECT A1C_OK_VDI_CORRUPT_REJECT; do
  grep -aq "$marker" "$work/serial.log" || fail "missing marker $marker"
done
if grep -aq A1C_ERROR_ "$work/serial.log"; then
  grep -a A1C_ERROR_ "$work/serial.log" >&2
  fail "GRUB VDI behavior failed"
fi
test "$(grep -aoc A1IVE_FIXED_VDI_OK "$work/serial.log")" -ge 2 || fail "VDI payload not read twice"
passed=1
echo 'PASS: actual fixed VDI and legacy 2MiB VDI FAT reads; dynamic and corrupt rejected'
