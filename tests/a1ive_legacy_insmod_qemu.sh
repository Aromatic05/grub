#!/bin/sh
# a1ive legacy module-name compatibility smoke test (real GRUB EFI/QEMU).
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for tool in qemu-system-x86_64 timeout mkfs.vfat mmd mcopy truncate; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
for f in "$build/grub-mkstandalone" "$code" "$vars"; do
  test -f "$f" || { echo "Missing $f" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-old-modules.XXXXXX")
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

cat > "$work/grub.cfg" <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set root=hd0
echo A1C_OLD_MODULES_BEGIN
insmod ls
echo A1C_DISKS_BEFORE
ls
if insmod fixed_vdi; then echo A1C_OK_FIXED_VDI; else echo A1C_ERROR_FIXED_VDI; fi
echo A1C_DISKS_AFTER_FIXED
ls
if insmod lynxfs; then echo A1C_OK_LYNXFS; else echo A1C_ERROR_LYNXFS; fi
if insmod qnx4; then echo A1C_OK_QNX4; else echo A1C_ERROR_QNX4; fi
if insmod qnx6; then echo A1C_OK_QNX6; else echo A1C_ERROR_QNX6; fi
if insmod shim_lock; then echo A1C_OK_SHIM_LOCK; else echo A1C_ERROR_SHIM_LOCK; fi
if insmod sortlib; then echo A1C_OK_SORTLIB; else echo A1C_ERROR_SORTLIB; fi
echo A1C_DISKS_AFTER_SORTLIB
ls
if insmod xhci; then echo A1C_OK_XHCI; else echo A1C_ERROR_XHCI; fi
echo A1C_DISKS_AFTER_XHCI
ls
if insmod usbms; then echo A1C_OK_USBMS; else echo A1C_ERROR_USBMS; fi
echo A1C_DISKS_AFTER_USBMS
ls
if insmod vhd; then echo A1C_OK_VHD_PIPELINE; else echo A1C_ERROR_VHD_PIPELINE; fi
echo A1C_DISKS_AFTER
ls
if probe --fs --set=fsname (hd0); then
  if [ "$fsname" = fat ]; then echo A1C_OK_ESP_FAT; else echo A1C_ERROR_ESP_FS; fi
else
  echo A1C_ERROR_ESP_PROBE
fi
echo A1C_OLD_MODULES_DONE
halt
GRUB
truncate -s 40M "$work/esp.img"
mkfs.vfat -F 16 -n OLDGRUB "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT

mods='serial terminal normal echo test halt fat part_msdos probe ls fixed_vdi vhd lynxfs qnx4 qnx6 shim_lock sortlib xhci usb usbms'
preload='serial terminal normal echo test halt fat part_msdos probe'
if ! "$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules="$mods" --modules="$preload" \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1; then
  tail -n 24 "$work/pack.log" >&2
  fail "cannot package all seven old module names"
fi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$vars" "$work/vars.fd"
if ! timeout 65s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  > "$work/qemu.log" 2>&1; then
  fail "QEMU guest did not exit cleanly"
fi
for marker in A1C_OLD_MODULES_BEGIN A1C_OLD_MODULES_DONE \
 A1C_OK_FIXED_VDI A1C_OK_LYNXFS A1C_OK_QNX4 A1C_OK_QNX6 \
 A1C_OK_SHIM_LOCK A1C_OK_SORTLIB A1C_OK_XHCI A1C_OK_USBMS \
 A1C_OK_VHD_PIPELINE A1C_OK_ESP_FAT; do
 grep -aq "$marker" "$work/serial.log" || fail "guest marker missing $marker"
done
if grep -aq A1C_ERROR_ "$work/serial.log"; then
  grep -a A1C_ERROR_ "$work/serial.log" >&2
  fail "one or more legacy module operations failed"
fi
passed=1
echo 'PASS: seven a1ive insmod module names registered in EFI QEMU, FAT still readable'
