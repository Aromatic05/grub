#!/bin/sh
# Check that EFI module pages can be reused after rmmod.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${GRUB_BUILD:-"$repo/build-x86_64-efi"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}

for tool in mkfs.vfat mmd mcopy qemu-system-x86_64 timeout; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
for file in "$build/grub-mkstandalone" "$build/grub-core/hello.mod" \
            "$build/grub-core/moddep.lst" "$ovmf_code" "$ovmf_vars"; do
  test -f "$file" || { echo "Missing $file" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/grub-efi-reload.XXXXXX")
pass=0
cleanup()
{
  if test "$pass" = 1 && test "${EFI_MODULE_RELOAD_KEEP:-0}" != 1; then
    rm -rf "$work"
  else
    echo "EFI module reload evidence: $work" >&2
  fi
}
trap cleanup EXIT HUP INT TERM

cat > "$work/grub.cfg" <<'GRUBCFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set prefix=(hd0)/boot/grub
echo EFI_RELOAD_BEGIN
if insmod hello; then echo EFI_RELOAD_LOAD_1; else echo EFI_RELOAD_FAIL_LOAD_1; fi
if rmmod hello; then echo EFI_RELOAD_UNLOAD_1; else echo EFI_RELOAD_FAIL_UNLOAD_1; fi
if insmod hello; then echo EFI_RELOAD_LOAD_2; else echo EFI_RELOAD_FAIL_LOAD_2; fi
if rmmod hello; then echo EFI_RELOAD_UNLOAD_2; else echo EFI_RELOAD_FAIL_UNLOAD_2; fi
echo EFI_RELOAD_DONE
halt
GRUBCFG

"$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --compress=no \
  --modules='serial terminal normal echo halt minicmd test part_msdos fat' \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1
truncate -s 32M "$work/esp.img"
mkfs.vfat -F 16 -n GRUBRELOAD "$work/esp.img" > "$work/mkfs.log" 2>&1
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT ::/boot ::/boot/grub \
    ::/boot/grub/x86_64-efi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$work/esp.img" "$build/grub-core/hello.mod" \
  ::/boot/grub/x86_64-efi/hello.mod
mcopy -i "$work/esp.img" "$build/grub-core/moddep.lst" \
  ::/boot/grub/x86_64-efi/moddep.lst
cp "$ovmf_vars" "$work/OVMF_VARS.fd"

timeout 35s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -monitor none -no-reboot \
  -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/OVMF_VARS.fd" \
  -drive "if=virtio,format=raw,readonly=on,file=$work/esp.img" \
  > "$work/qemu.log" 2>&1 || {
    echo "QEMU did not exit cleanly" >&2
    exit 1
  }

for marker in EFI_RELOAD_BEGIN EFI_RELOAD_LOAD_1 EFI_RELOAD_UNLOAD_1 \
              EFI_RELOAD_LOAD_2 EFI_RELOAD_UNLOAD_2 EFI_RELOAD_DONE; do
  grep -aq "$marker" "$work/serial.log" || {
    echo "Missing GRUB marker $marker" >&2
    exit 1
  }
done
if grep -aq 'EFI_RELOAD_FAIL_\|#PF' "$work/serial.log"; then
  echo "EFI module reload failed" >&2
  exit 1
fi

pass=1
echo "PASS: hello.mod insmod/rmmod twice under UEFI QEMU"
