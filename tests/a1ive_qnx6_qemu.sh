#!/bin/sh
# QNX6 EFI GRUB: positive short-name inode-file read and CRC-negative test.
# Synthetic fixture is built from the QNX6 on-disk structs, not QNX mkfs.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for tool in python3 qemu-system-x86_64 timeout mkfs.vfat mcopy mmd truncate; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
for file in "$build/grub-mkstandalone" "$build/grub-core/qnx6.mod" "$code" "$vars"; do
  test -s "$file" || { echo "Missing $file" >&2; exit 1; }
done
work=${QNX6_WORK_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/a1ive-qnx6.XXXXXX")}
mkdir -p "$work"
passed=0
cleanup() {
  if test "$passed" = 1 && test "${A1C_KEEP:-0}" != 1 && test -z "${QNX6_WORK_DIR:-}"; then
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

work = pathlib.Path(sys.argv[1])
sector = 512
image = bytearray(128 * sector)
base = 0x3000
payload = b"A1IVE_QNX6_FILE_READ_OK\n"

def inode(size, mode, block):
    b = bytearray(128)
    struct.pack_into("<Q", b, 0, size)
    struct.pack_into("<H", b, 32, mode)
    struct.pack_into("<I", b, 36, block)
    b[100] = 0
    return b

# The inode-file contains inode 1 (root dir) and inode 2 (marker).
inode_table = inode(32, 0o040755, 2) + inode(len(payload), 0o100644, 3)
image[base + sector:base + sector + len(inode_table)] = inode_table
dirent = bytearray(32)
struct.pack_into("<I", dirent, 0, 2)
dirent[4] = len(b"MARKER.TXT")
dirent[5:5 + len(b"MARKER.TXT")] = b"MARKER.TXT"
image[base + 2 * sector:base + 2 * sector + 32] = dirent
image[base + 3 * sector:base + 3 * sector + len(payload)] = payload

def superblock(serial):
    b = bytearray(512)
    struct.pack_into("<IIQ", b, 0, 0x68191122, 0, serial)
    struct.pack_into("<I", b, 48, 512)
    struct.pack_into("<I", b, 52, 2)
    struct.pack_into("<I", b, 60, 128)
    struct.pack_into("<Q", b, 72, 256)
    struct.pack_into("<I", b, 80, 1)
    b[136] = 0
    crc = 0
    for byte in b[8:]:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
    struct.pack_into("<I", b, 4, crc)
    return b

image[0x2000:0x2200] = superblock(1)
image[-4096:-3584] = superblock(2)
(work / "qnx6.img").write_bytes(image)
corrupt = bytearray(image)
corrupt[0x2000 + 4] ^= 0xff
corrupt[len(corrupt) - 4096 + 4] ^= 0xff
(work / "corrupt.img").write_bytes(corrupt)
PY

cat > "$work/grub.cfg" <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo A1C_QNX6_BEGIN
if insmod qnx6; then echo A1C_OK_QNX6_MODULE; else echo A1C_ERROR_QNX6_MODULE; fi
if insmod loopback; then echo A1C_OK_QNX6_LOOPBACK; else echo A1C_ERROR_QNX6_LOOPBACK; fi
if loopback q6 (hd0)/qnx6.img; then
  if ls (q6)/; then echo A1C_OK_QNX6_DIR; else echo A1C_ERROR_QNX6_DIR; fi
  if cat (q6)/MARKER.TXT; then echo A1C_OK_QNX6_FILE; else echo A1C_ERROR_QNX6_FILE; fi
  loopback -d q6
else
  echo A1C_ERROR_QNX6_OPEN
fi
if loopback qbad (hd0)/corrupt.img; then
  if cat (qbad)/MARKER.TXT; then echo A1C_ERROR_QNX6_BAD_ACCEPT; else echo A1C_OK_QNX6_BAD_REJECT; fi
  loopback -d qbad
else
  echo A1C_OK_QNX6_BAD_REJECT
fi
echo A1C_QNX6_DONE
halt
GRUB
truncate -s 24M "$work/esp.img"
mkfs.vfat -F 16 -n QNX6ROOT "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$work/qnx6.img" ::/qnx6.img
mcopy -i "$work/esp.img" "$work/corrupt.img" ::/corrupt.img
if ! "$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules='serial terminal normal echo test halt fat cat ls loopback qnx6' \
  --modules='serial terminal normal echo test halt fat cat ls' \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1; then
  tail -n 16 "$work/pack.log" >&2
  fail "cannot package QNX6 GRUB EFI"
fi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$vars" "$work/vars.fd"
qrc=0
timeout 38s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  > "$work/qemu.log" 2>&1 || qrc=$?
test "$qrc" = 0 || fail "QEMU exit $qrc"
for marker in A1C_QNX6_BEGIN A1C_QNX6_DONE A1C_OK_QNX6_MODULE \
  A1C_OK_QNX6_LOOPBACK A1C_OK_QNX6_DIR A1C_OK_QNX6_FILE \
  A1C_OK_QNX6_BAD_REJECT A1IVE_QNX6_FILE_READ_OK; do
  grep -aq "$marker" "$work/serial.log" || fail "missing guest marker $marker"
done
if grep -aq A1C_ERROR_ "$work/serial.log"; then
  grep -a A1C_ERROR_ "$work/serial.log" >&2
  fail "GRUB QNX6 operation failed"
fi
passed=1
echo 'PASS: GRUB EFI QNX6 dual superblock CRC directory/inode file read, corrupt CRC rejected'
