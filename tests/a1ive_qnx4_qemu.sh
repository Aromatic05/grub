#!/bin/sh
# GRUB QNX4 driver positive file-read and invalid-root regression.
# Fixture uses the documented 64-byte little-endian inode and 512-byte extents.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for tool in python3 qemu-system-x86_64 timeout mkfs.vfat mcopy mmd truncate; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
for path in "$build/grub-mkstandalone" "$build/grub-core/qnx4.mod" "$code" "$vars"; do
  test -s "$path" || { echo "Missing $path" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-qnx4.XXXXXX")
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

python3 - "$work" <<'PY'
import pathlib
import struct
import sys

directory = pathlib.Path(sys.argv[1])
data = bytearray(8 * 1024 * 1024)
mark = b"A1IVE_QNX4_FILE_READ_OK\n"

def inode(sector, idx, name, size, first_extent_block, mode):
    # QNX4 physical block values are one-based, while grub_disk_read
    # sectors are zero-based.  Root inode #8 is in physical sector 1.
    off = sector * 512 + idx * 64
    assert len(name) <= 16
    data[off:off + len(name)] = name
    struct.pack_into("<I", data, off + 0x10, size)
    struct.pack_into("<II", data, off + 0x14, first_extent_block, 1)
    struct.pack_into("<H", data, off + 0x30, 1)
    struct.pack_into("<H", data, off + 0x32, mode)
    struct.pack_into("<H", data, off + 0x38, 1)
    data[off + 0x3F] = 0x11 # USED | INODE

# / is inode #8 in sector 1; its directory data is sector 3.
inode(1, 0, b"/", 64, 4, 0o040755)
# The 64-byte inode doubles as the directory entry in sector 3.
inode(3, 0, b"MARKER.TXT", len(mark), 5, 0o100644)
# Extent physical block 5 maps to zero-based disk sector 4.
data[4 * 512:4 * 512 + len(mark)] = mark

(directory / "qnx4.img").write_bytes(data)
bad = bytearray(data)
bad[512] = ord("!")
(directory / "corrupt.img").write_bytes(bad)
PY

cat > "$work/grub.cfg" <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo A1C_QNX4_BEGIN
if insmod qnx4; then echo A1C_OK_QNX4_MODULE; else echo A1C_ERROR_QNX4_MODULE; fi
if insmod loopback; then echo A1C_OK_QNX4_LOOPBACK; else echo A1C_ERROR_QNX4_LOOPBACK; fi
if loopback qnx4img (hd0)/qnx4.img; then
  if ls (qnx4img)/; then echo A1C_OK_QNX4_ROOT; else echo A1C_ERROR_QNX4_ROOT; fi
  if cat (qnx4img)/MARKER.TXT; then echo A1C_OK_QNX4_FILE; else echo A1C_ERROR_QNX4_FILE; fi
  loopback -d qnx4img
else
  echo A1C_ERROR_QNX4_OPEN
fi
if loopback qnx4bad (hd0)/corrupt.img; then
  if cat (qnx4bad)/MARKER.TXT; then echo A1C_ERROR_QNX4_BAD_ACCEPT; else echo A1C_OK_QNX4_BAD_REJECT; fi
  loopback -d qnx4bad
else
  echo A1C_OK_QNX4_BAD_REJECT
fi
echo A1C_QNX4_DONE
halt
GRUB

truncate -s 32M "$work/esp.img"
mkfs.vfat -F 16 -n QNX4ROOT "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$work/qnx4.img" ::/qnx4.img
mcopy -i "$work/esp.img" "$work/corrupt.img" ::/corrupt.img
"$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules='serial terminal normal echo test halt fat cat ls loopback qnx4' \
  --modules='serial terminal normal echo test halt fat cat ls' \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1 || { tail -n 16 "$work/pack.log" >&2; fail "cannot pack GRUB"; }
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$vars" "$work/vars.fd"
qrc=0
timeout 37s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  > "$work/qemu.log" 2>&1 || qrc=$?
test "$qrc" = 0 || fail "QEMU exit $qrc"
for marker in A1C_QNX4_BEGIN A1C_QNX4_DONE A1C_OK_QNX4_MODULE \
  A1C_OK_QNX4_LOOPBACK A1C_OK_QNX4_ROOT A1C_OK_QNX4_FILE A1C_OK_QNX4_BAD_REJECT \
  A1IVE_QNX4_FILE_READ_OK; do
  grep -aq "$marker" "$work/serial.log" || fail "missing guest marker $marker"
done
if grep -aq A1C_ERROR_ "$work/serial.log"; then
  grep -a A1C_ERROR_ "$work/serial.log" >&2
  fail "GRUB reported QNX4 failure"
fi
passed=1
echo 'PASS: GRUB EFI QNX4 directory & physical 512B extent file read, corrupt root rejected'
