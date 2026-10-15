#!/bin/sh
# LynxOS-178 LynxFS: native GRUB EFI directory and actual file-read regression.
# Generates structurally valid 1024/2048-byte-block big-endian disk fixtures.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for tool in python3 qemu-system-x86_64 timeout mkfs.vfat mcopy mmd truncate; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
for file in "$build/grub-mkstandalone" "$build/grub-core/lynxfs.mod" "$code" "$vars"; do
  test -s "$file" || { echo "Missing $file" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-lynxfs.XXXXXX")
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

def make_image(block_size):
    assert block_size in (1024, 2048)
    data = bytearray(8 * 1024 * 1024)
    marker = f"A1IVE_LYNXFS_{block_size}_FILE_READ_OK\n".encode()
    superblock = bytearray(84)
    # LynxFS superblock is 21 big-endian uint32_t fields.
    struct.pack_into(">I", superblock, 8, 16)  # num_inodes
    struct.pack_into(">I", superblock, 12, len(data) // block_size)
    struct.pack_into(">I", superblock, 16, 1)  # root inode no.
    struct.pack_into(">I", superblock, 40, 0x11112222)  # magic
    struct.pack_into(">I", superblock, 48, 0xCAFEFECA)  # bsize magic
    struct.pack_into(">I", superblock, 52, block_size)
    struct.pack_into(">I", superblock, 56, 0x1FEDFACE)  # bitmap magic
    # Block zero stores an early shadow superblock at byte offset 512.
    data[512:512 + len(superblock)] = superblock
    # Real superblock is block 1.
    data[block_size:block_size + len(superblock)] = superblock

    def put_inode(ino, mode, size, first_block):
        # Inodes are 128-byte records starting at block 2; index is 1-based.
        off = block_size * 2 + (ino - 1) * 128
        struct.pack_into(">HH", data, off, mode, 1)  # mode and nlink
        struct.pack_into(">I", data, off + 12, size)
        struct.pack_into(">I", data, off + 28, 1)  # number allocated blocks
        data[off + 32:off + 35] = first_block.to_bytes(3, "big")

    put_inode(1, 0o040755, block_size, 4)
    put_inode(2, 0o100644, len(marker), 5)

    # Directory entry: inode, total record length, name length, name bytes.
    off = block_size * 4
    struct.pack_into(">IHH", data, off, 2, block_size, len(b"MARKER.TXT"))
    data[off + 8:off + 8 + len(b"MARKER.TXT")] = b"MARKER.TXT"
    data[block_size * 5:block_size * 5 + len(marker)] = marker
    return data

(directory / "lynx1024.img").write_bytes(make_image(1024))
(directory / "lynx2048.img").write_bytes(make_image(2048))
bad = make_image(1024)
bad[512 + 40] ^= 0xFF
(directory / "corrupt.img").write_bytes(bad)
PY

cat > "$work/grub.cfg" <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo A1C_LYNX_BEGIN
if insmod lynxfs; then echo A1C_OK_LYNX_MODULE; else echo A1C_ERROR_LYNX_MODULE; fi
if insmod loopback; then echo A1C_OK_LYNX_LOOPBACK; else echo A1C_ERROR_LYNX_LOOPBACK; fi
if loopback lx1 (hd0)/lynx1024.img; then
  if ls (lx1)/; then echo A1C_OK_LYNX1024_DIR; else echo A1C_ERROR_LYNX1024_DIR; fi
  if cat (lx1)/MARKER.TXT; then echo A1C_OK_LYNX1024_FILE; else echo A1C_ERROR_LYNX1024_FILE; fi
  loopback -d lx1
else
  echo A1C_ERROR_LYNX1024_MOUNT
fi
if loopback lx2 (hd0)/lynx2048.img; then
  if ls (lx2)/; then echo A1C_OK_LYNX2048_DIR; else echo A1C_ERROR_LYNX2048_DIR; fi
  if cat (lx2)/MARKER.TXT; then echo A1C_OK_LYNX2048_FILE; else echo A1C_ERROR_LYNX2048_FILE; fi
  loopback -d lx2
else
  echo A1C_ERROR_LYNX2048_MOUNT
fi
if loopback lxbad (hd0)/corrupt.img; then
  if cat (lxbad)/MARKER.TXT; then echo A1C_ERROR_LYNX_BAD_ACCEPT; else echo A1C_OK_LYNX_BAD_REJECT; fi
  loopback -d lxbad
else
  echo A1C_OK_LYNX_BAD_REJECT
fi
echo A1C_LYNX_DONE
halt
GRUB

truncate -s 64M "$work/esp.img"
mkfs.vfat -F 16 -n LYNXROOT "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
for name in lynx1024 lynx2048 corrupt; do
  mcopy -i "$work/esp.img" "$work/$name.img" "::$name.img"
done
if ! "$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules='serial terminal normal echo test halt fat cat ls loopback lynxfs' \
  --modules='serial terminal normal echo test halt fat cat ls' \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1; then
  tail -n 15 "$work/pack.log" >&2
  fail "cannot package standalone LynxFS GRUB"
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
for marker in A1C_LYNX_BEGIN A1C_LYNX_DONE A1C_OK_LYNX_MODULE A1C_OK_LYNX_LOOPBACK \
  A1C_OK_LYNX1024_DIR A1C_OK_LYNX1024_FILE \
  A1C_OK_LYNX2048_DIR A1C_OK_LYNX2048_FILE A1C_OK_LYNX_BAD_REJECT \
  A1IVE_LYNXFS_1024_FILE_READ_OK A1IVE_LYNXFS_2048_FILE_READ_OK; do
  grep -aq "$marker" "$work/serial.log" || fail "missing guest marker $marker"
done
if grep -aq A1C_ERROR_ "$work/serial.log"; then
  grep -a A1C_ERROR_ "$work/serial.log" >&2
  fail "GRUB LynxFS read/reject error"
fi
passed=1
echo 'PASS: LynxFS 1024/2048B big-endian inode/dir/extent FAT read, bad magic rejected'
