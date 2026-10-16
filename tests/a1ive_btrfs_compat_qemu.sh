#!/bin/sh
# Real offline Btrfs subvolume/metadata regression in OVMF QEMU.
# Uses mkfs.btrfs --rootdir --subvol; never mounts a host device.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
efi_build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for exe in mkfs.btrfs qemu-system-x86_64 timeout mkfs.vfat mmd mcopy truncate; do
  command -v "$exe" >/dev/null 2>&1 || { echo "Missing $exe" >&2; exit 1; }
done
for f in "$efi_build/grub-mkstandalone" "$ovmf_code" "$ovmf_vars"; do
  test -f "$f" || { echo "Missing $f" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-btrfs.XXXXXX")
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
mkdir -p "$work/rootdir/rootfs" "$work/rootdir/other"
printf 'A1C_BTRFS_DEFAULT_OK' > "$work/rootdir/rootfs/default.txt"
printf 'A1C_BTRFS_OTHER_OK' > "$work/rootdir/other/other.txt"
truncate -s 256M "$work/btrfs.img"
if ! mkfs.btrfs -f -L A1CBTRFS -m single -d single \
  -r "$work/rootdir" -u default:rootfs -u rw:other \
  "$work/btrfs.img" > "$work/mkfs-btrfs.log" 2>&1; then
  tail -n 15 "$work/mkfs-btrfs.log" >&2
  fail 'could not construct offline Btrfs subvolume image'
fi
truncate -s 48M "$work/esp.img"
mkfs.vfat -F 16 -n A1CBTRBOOT "$work/esp.img" > "$work/mkfs-fat.log" 2>&1
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT

cat > "$work/grub.cfg" <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set root=hd0
echo A1C_BTRFS_BEGIN
if insmod btrfs; then echo A1C_OK_INSMOD_BTRFS; else echo A1C_ERROR_INSMOD_BTRFS; fi
if insmod cat; then echo A1C_OK_INSMOD_CAT; else echo A1C_ERROR_INSMOD_CAT; fi

if btrfs-info (hd1); then echo A1C_OK_BTRFS_INFO; else echo A1C_ERROR_BTRFS_INFO; fi
if btrfs-get-default-subvol -i -o default_id (hd1); then
  if [ "$default_id" = "257" ]; then echo A1C_OK_BTRFS_DEFAULT_ID; else echo "A1C_ERROR_DEFAULT_ID=$default_id"; fi
else
  echo A1C_ERROR_BTRFS_DEFAULT_CMD
fi
if btrfs-get-default-subvol -p -o default_path (hd1); then
  if [ "$default_path" = "/rootfs" ]; then echo A1C_OK_BTRFS_DEFAULT_PATH; else echo "A1C_ERROR_DEFAULT_PATH=$default_path"; fi
else
  echo A1C_ERROR_BTRFS_DEFAULT_PATH_CMD
fi
if btrfs-get-default-subvol -p -i -o idpath (hd1); then
  if [ "$idpath" = "257 /rootfs" ]; then echo A1C_OK_BTRFS_DEFAULT_IDPATH; else echo "A1C_ERROR_DEFAULT_IDPATH=$idpath"; fi
else
  echo A1C_ERROR_BTRFS_DEFAULT_IDPATH_CMD
fi

if btrfs-list-subvols -i -o all_ids (hd1); then
  if [ -n "$all_ids" ]; then echo A1C_OK_BTRFS_LIST_IDS; else echo A1C_ERROR_BTRFS_LIST_EMPTY; fi
  echo "A1C_LIST_IDS=$all_ids"
else
  echo A1C_ERROR_BTRFS_LIST_CMD
fi
if btrfs-list-subvols -p -o all_paths (hd1); then
  if [ -n "$all_paths" ]; then echo A1C_OK_BTRFS_LIST_PATHS; else echo A1C_ERROR_BTRFS_PATHS_EMPTY; fi
  echo "A1C_LIST_PATHS=$all_paths"
else
  echo A1C_ERROR_BTRFS_LIST_PATHS_CMD
fi
if btrfs-list-subvols (hd1); then echo A1C_OK_BTRFS_LIST_DEFAULT; else echo A1C_ERROR_BTRFS_LIST_DEFAULT; fi

if btrfs-get-default-subvol (hd1) legacy_id; then
  if [ "$legacy_id" = "$default_path" ]; then echo A1C_OK_BTRFS_LEGACY_VAR; else echo A1C_ERROR_BTRFS_LEGACY_VAR; fi
else
  echo A1C_ERROR_BTRFS_LEGACY_CMD
fi

if cat --set=found2 (hd1)/rootfs/default.txt; then
  if [ "$found2" = "A1C_BTRFS_DEFAULT_OK" ]; then echo A1C_OK_BTRFS_ROOTFS_READ; else echo A1C_ERROR_BTRFS_ROOTFS_CONTENT; fi
else
  echo A1C_ERROR_BTRFS_ROOTFS_OPEN
fi
if cat --set=found3 (hd1)/other/other.txt; then
  if [ "$found3" = "A1C_BTRFS_OTHER_OK" ]; then echo A1C_OK_BTRFS_SECOND_READ; else echo A1C_ERROR_BTRFS_OTHER_CONTENT; fi
else
  echo A1C_ERROR_BTRFS_OTHER_OPEN
fi

# The mount table must redirect file opens to the selected subvolume tree,
# without replacing unrelated paths or accepting invalid mounts.
if btrfs-mount-subvol (hd1) /other /rootfs; then echo A1C_OK_BTRFS_MOUNT; else echo A1C_ERROR_BTRFS_MOUNT; fi
if cat --set=mounted (hd1)/other/default.txt; then
  if [ "$mounted" = "A1C_BTRFS_DEFAULT_OK" ]; then echo A1C_OK_BTRFS_MOUNT_READ; else echo A1C_ERROR_BTRFS_MOUNT_CONTENT; fi
else
  echo A1C_ERROR_BTRFS_MOUNT_OPEN
fi
if btrfs-mount-subvol (hd1) /missing-dir /rootfs; then echo A1C_ERROR_BTRFS_BAD_MOUNT_DIR; else echo A1C_OK_BTRFS_BAD_MOUNT_DIR; fi
if btrfs-mount-subvol (hd1) /other /missing-subvol; then echo A1C_ERROR_BTRFS_BAD_MOUNT_SUBVOL; else echo A1C_OK_BTRFS_BAD_MOUNT_SUBVOL; fi

# Both historical environment hooks must change the root used by file open.
set btrfs_subvolid=257
if cat --set=by_id (hd1)/default.txt; then
  if [ "$by_id" = "A1C_BTRFS_DEFAULT_OK" ]; then echo A1C_OK_BTRFS_SELECT_ID; else echo A1C_ERROR_BTRFS_SELECT_ID_CONTENT; fi
else
  echo A1C_ERROR_BTRFS_SELECT_ID_OPEN
fi
set btrfs_subvol=other
if cat --set=by_name (hd1)/other.txt; then
  if [ "$by_name" = "A1C_BTRFS_OTHER_OK" ]; then echo A1C_OK_BTRFS_SELECT_NAME; else echo A1C_ERROR_BTRFS_SELECT_NAME_CONTENT; fi
else
  echo A1C_ERROR_BTRFS_SELECT_NAME_OPEN
fi
set btrfs_subvol=
set btrfs_subvolid=
set btrfs_relative_path=1
if cat --set=by_relative (hd1)/default.txt; then
  if [ "$by_relative" = "A1C_BTRFS_DEFAULT_OK" ]; then echo A1C_OK_BTRFS_RELATIVE; else echo A1C_ERROR_BTRFS_RELATIVE_CONTENT; fi
else
  echo A1C_ERROR_BTRFS_RELATIVE_OPEN
fi
set btrfs_relative_path=
echo A1C_BTRFS_DONE
halt
GRUB

mods='serial terminal normal echo test halt fat part_msdos btrfs cat'
preloaded='serial terminal normal echo test halt fat part_msdos'
if ! "$efi_build/grub-mkstandalone" -O x86_64-efi -d "$efi_build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules="$mods" --modules="$preloaded" \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1; then
  tail -n 12 "$work/pack.log" >&2
  fail 'could not package GRUB Btrfs module into standalone EFI'
fi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$ovmf_vars" "$work/vars.fd"
if ! timeout 75s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  -drive "if=virtio,format=raw,file=$work/btrfs.img" \
  > "$work/qemu.log" 2>&1; then
  fail 'Btrfs QEMU did not exit cleanly'
fi
log="$work/serial.log"
for token in A1C_BTRFS_BEGIN A1C_BTRFS_DONE \
  A1C_OK_INSMOD_BTRFS A1C_OK_INSMOD_CAT A1C_OK_BTRFS_INFO \
  A1C_OK_BTRFS_DEFAULT_ID A1C_OK_BTRFS_DEFAULT_PATH A1C_OK_BTRFS_DEFAULT_IDPATH \
  A1C_OK_BTRFS_LIST_IDS A1C_OK_BTRFS_LIST_PATHS A1C_OK_BTRFS_LIST_DEFAULT \
  A1C_OK_BTRFS_LEGACY_VAR \
  A1C_OK_BTRFS_ROOTFS_READ A1C_OK_BTRFS_SECOND_READ \
  A1C_OK_BTRFS_MOUNT A1C_OK_BTRFS_MOUNT_READ \
  A1C_OK_BTRFS_BAD_MOUNT_DIR A1C_OK_BTRFS_BAD_MOUNT_SUBVOL \
  A1C_OK_BTRFS_SELECT_ID A1C_OK_BTRFS_SELECT_NAME A1C_OK_BTRFS_RELATIVE; do
  grep -aq "$token" "$log" || fail "missing marker: $token"
done
if grep -aq A1C_ERROR_ "$log"; then
  grep -a A1C_ERROR_ "$log" >&2
  fail 'Btrfs command assertion failed'
fi
grep -aq "Label: 'A1CBTRFS'" "$log" || fail 'btrfs-info did not report fixture label'
grep -aq 'A1C_LIST_IDS=256' "$log" || fail 'Btrfs list did not include subvol 256'
grep -aq '257' "$log" || fail 'Btrfs list did not include second subvol 257'
grep -aq '/rootfs' "$log" || fail 'Btrfs -p did not include rootfs path'
grep -aq '/other' "$log" || fail 'Btrfs -p did not include other path'
grep -aq 'ID 257 path /rootfs' "$log" || fail 'Btrfs default format did not report ID and path'
passed=1
echo 'PASS: Btrfs default/list/path, mount redirection, variable root selection'
