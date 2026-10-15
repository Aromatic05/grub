#!/bin/sh
# Native QEMU xHCI -> USB mass storage -> SCSI -> FAT end-to-end regression.
# The file must be read from (usb0), not the EFI firmware Block I/O disk.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for tool in qemu-system-x86_64 timeout mkfs.vfat mmd mcopy truncate; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
for file in "$build/grub-mkstandalone" "$code" "$vars"; do
  test -f "$file" || { echo "Missing $file" >&2; exit 1; }
done
for mod in xhci usb usbms scsi fat cat probe; do
  test -s "$build/grub-core/$mod.mod" || { echo "Missing module $mod" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-xhci-usb3.XXXXXX")
pass=0
cleanup() {
  if test "$pass" = 1 && test "${A1C_KEEP:-0}" != 1; then
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
echo A1C_XHCI_BEGIN
if probe --fs -s rootfs (hd0); then
  if [ "$rootfs" = fat ]; then echo A1C_OK_FIRMWARE_BEFORE; else echo A1C_ERROR_FIRMWARE_BEFORE; fi
else
  echo A1C_ERROR_FIRMWARE_BEFORE
fi
if insmod xhci; then echo A1C_OK_XHCI_MODULE; else echo A1C_ERROR_XHCI_MODULE; fi
if insmod usbms; then echo A1C_OK_USBMS_MODULE; else echo A1C_ERROR_USBMS_MODULE; fi
if insmod scsi; then echo A1C_OK_SCSI_MODULE; else echo A1C_ERROR_SCSI_MODULE; fi
if cat (usb0)/MARKER.TXT; then
  echo A1C_OK_NATIVE_USB3_FILE
else
  echo A1C_ERROR_NATIVE_USB3_FILE
fi
if probe --fs -s rootfs (hd0); then
  if [ "$rootfs" = fat ]; then echo A1C_OK_FIRMWARE_AFTER; else echo A1C_ERROR_FIRMWARE_AFTER; fi
else
  echo A1C_ERROR_FIRMWARE_AFTER
fi
echo A1C_XHCI_DONE
halt
GRUB

if test -n "${A1C_GRUB_DEBUG:-}"; then
  sed -i "/^set pager=0$/a set debug=$A1C_GRUB_DEBUG" "$work/grub.cfg"
fi

truncate -s 32M "$work/esp.img"
mkfs.vfat -F 16 -n XHCIESP "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
truncate -s 32M "$work/usb.img"
mkfs.vfat -F 16 -n XHCIUSB "$work/usb.img" >/dev/null
printf 'A1IVE_NATIVE_USB3_FAT_MARKER_OK\n' > "$work/MARKER.TXT"
mcopy -i "$work/usb.img" "$work/MARKER.TXT" ::/MARKER.TXT

if ! "$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules='serial terminal normal echo test halt fat cat probe xhci usb usbms scsi' \
  --modules='serial terminal normal echo test halt fat cat probe' \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1; then
  tail -n 18 "$work/pack.log" >&2
  fail "cannot package xHCI EFI module"
fi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$vars" "$work/vars.fd"
qemu_rc=0
timeout 35s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  -device qemu-xhci,id=usb3 \
  -drive "if=none,id=usbdata,format=raw,readonly=on,file=$work/usb.img" \
  -device usb-storage,bus=usb3.0,drive=usbdata \
  > "$work/qemu.log" 2>&1 || qemu_rc=$?
test "$qemu_rc" = 0 || fail "QEMU exit status $qemu_rc"
for marker in A1C_XHCI_BEGIN A1C_XHCI_DONE A1C_OK_FIRMWARE_BEFORE \
  A1C_OK_FIRMWARE_AFTER A1C_OK_XHCI_MODULE A1C_OK_USBMS_MODULE \
  A1C_OK_SCSI_MODULE A1C_OK_NATIVE_USB3_FILE \
  A1IVE_NATIVE_USB3_FAT_MARKER_OK; do
  grep -aq "$marker" "$work/serial.log" || fail "missing guest marker $marker"
done
if grep -aq A1C_ERROR_ "$work/serial.log"; then
  grep -a A1C_ERROR_ "$work/serial.log" >&2
  fail "GRUB native USB3 or firmware disk operation failed"
fi
pass=1
echo 'PASS: xHCI native usb0 FAT file read, EFI firmware disk preserved before and after'
