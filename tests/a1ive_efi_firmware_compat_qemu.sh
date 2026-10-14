#!/bin/sh
# EFI-only a1ive compatibility regression for BLS, firmware variable and ACPI options.
# All NVRAM operations happen in a disposable copy of OVMF_VARS.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
efi_build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for tool in qemu-system-x86_64 timeout mkfs.vfat mmd mcopy truncate; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
for f in "$efi_build/grub-mkstandalone" "$code" "$vars"; do
  test -f "$f" || { echo "Missing $f" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-efi-firmware.XXXXXX")
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
vendor_guid=6f83c0d9-4982-4123-a990-ef56a331d77e

mkdir -p "$work/entries"
cat > "$work/entries/a1c-default.conf" <<'BLS'
title A1C BLS Test
version 1.0
linux /A1C-NOT-BOOTED
options test=a1c
BLS
printf 'bad' > "$work/bad.bmp"

cat > "$work/phase1.cfg" <<CFG
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set root=hd0
echo A1C_EFI_BEGIN

if insmod efivar; then echo A1C_OK_LOAD_EFIVAR; else echo A1C_ERROR_LOAD_EFIVAR; fi
if insmod bcfg; then echo A1C_OK_LOAD_BCFG; else echo A1C_ERROR_LOAD_BCFG; fi
if insmod blscfg; then echo A1C_OK_LOAD_BLSCFG; else echo A1C_ERROR_LOAD_BLSCFG; fi
if insmod acpi; then echo A1C_OK_LOAD_ACPI; else echo A1C_ERROR_LOAD_ACPI; fi

if setenv --guid $vendor_guid --type string A1C_VOL volatile; then echo A1C_OK_SET_VOL; else echo A1C_ERROR_SET_VOL; fi
if setenv -n -g $vendor_guid -t string A1C_PERSIST persistent; then echo A1C_OK_SET_NV; else echo A1C_ERROR_SET_NV; fi
if getenv --guid $vendor_guid --type string A1C_VOL tmp; then
  if [ "\$tmp" = "volatile" ]; then echo A1C_OK_GET_VOL; else echo "A1C_ERROR_GET_VOL=\$tmp"; fi
else
  echo A1C_ERROR_GET_VOL_CALL
fi
if getenv -g $vendor_guid -t string A1C_PERSIST tmp2; then
  if [ "\$tmp2" = "persistent" ]; then echo A1C_OK_GET_NV; else echo "A1C_ERROR_GET_NV=\$tmp2"; fi
else
  echo A1C_ERROR_GET_NV_CALL
fi

if bcfg help; then echo A1C_OK_BCFG_HELP; else echo A1C_ERROR_BCFG_HELP; fi

set default=a1c-default
if blscfg default; then echo A1C_OK_BLS_DEFAULT; else echo A1C_ERROR_BLS_DEFAULT; fi
if bls_import non-default; then echo A1C_OK_BLS_IMPORT; else echo A1C_ERROR_BLS_IMPORT; fi
if blscfg multiple positional; then echo A1C_ERROR_BLS_MULTI; else echo A1C_OK_BLS_MULTI_REJECT; fi

if acpi --slic; then echo A1C_ERROR_ACPI_SLIC_EMPTY; else echo A1C_OK_ACPI_SLIC_REJECT; fi
if acpi --bgrt (hd0)/bad.bmp; then echo A1C_ERROR_ACPI_BAD_BGRT; else echo A1C_OK_ACPI_BAD_BGRT; fi

echo A1C_EFI_DONE
halt
CFG

cat > "$work/phase2.cfg" <<CFG
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set root=hd0
insmod efivar
echo A1C_EFI_PHASE2
if getenv -g $vendor_guid -t string A1C_PERSIST persisted; then
  if [ "\$persisted" = "persistent" ]; then echo A1C_OK_NV_PERSISTED; else echo A1C_ERROR_NV_VALUE; fi
else
  echo A1C_ERROR_NV_NOT_PERSISTED
fi
if getenv -g $vendor_guid -t string A1C_VOL shouldbeabsent; then
  echo A1C_ERROR_VOL_PERSISTED
else
  echo A1C_OK_VOL_NOT_PERSISTED
fi
echo A1C_EFI_PHASE2_DONE
halt
CFG

truncate -s 48M "$work/esp.img"
mkfs.vfat -F 16 -n A1CEFI "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT ::/loader ::/loader/entries
mcopy -i "$work/esp.img" "$work/entries/a1c-default.conf" ::/loader/entries/a1c-default.conf
mcopy -i "$work/esp.img" "$work/bad.bmp" ::/bad.bmp
mods='serial terminal normal echo test halt fat part_msdos efivar bcfg blscfg blsuki acpi'
preloaded='serial terminal normal echo test halt fat part_msdos'
for phase in 1 2; do
  if ! "$efi_build/grub-mkstandalone" -O x86_64-efi -d "$efi_build/grub-core" \
    -o "$work/BOOTX64-$phase.EFI" --fonts='' --locales='' --themes='' --compress=no \
    --install-modules="$mods" --modules="$preloaded" \
    "boot/grub/grub.cfg=$work/phase$phase.cfg" > "$work/pack-$phase.log" 2>&1; then
    tail -n 12 "$work/pack-$phase.log" >&2
    fail "grub-mkstandalone phase$phase failed"
  fi
done
cp "$vars" "$work/vars.fd"

for phase in 1 2; do
  mcopy -o -i "$work/esp.img" "$work/BOOTX64-$phase.EFI" ::/EFI/BOOT/BOOTX64.EFI
  if ! timeout 60s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
    -m 512M -display none -no-reboot -serial "file:$work/serial-$phase.log" \
    -drive "if=pflash,format=raw,readonly=on,file=$code" \
    -drive "if=pflash,format=raw,file=$work/vars.fd" \
    -drive "if=virtio,format=raw,file=$work/esp.img" \
    > "$work/qemu-$phase.log" 2>&1; then
    fail "QEMU phase$phase did not exit cleanly"
  fi
done

for marker in A1C_EFI_BEGIN A1C_EFI_DONE \
 A1C_OK_LOAD_EFIVAR A1C_OK_LOAD_BCFG A1C_OK_LOAD_BLSCFG A1C_OK_LOAD_ACPI \
 A1C_OK_SET_VOL A1C_OK_SET_NV A1C_OK_GET_VOL A1C_OK_GET_NV \
 A1C_OK_BCFG_HELP A1C_OK_BLS_DEFAULT A1C_OK_BLS_IMPORT A1C_OK_BLS_MULTI_REJECT \
 A1C_OK_ACPI_SLIC_REJECT A1C_OK_ACPI_BAD_BGRT; do
  grep -aq "$marker" "$work/serial-1.log" || fail "phase1 missing marker $marker"
done
for marker in A1C_EFI_PHASE2 A1C_OK_NV_PERSISTED A1C_OK_VOL_NOT_PERSISTED A1C_EFI_PHASE2_DONE; do
  grep -aq "$marker" "$work/serial-2.log" || fail "phase2 missing marker $marker"
done
if grep -aq A1C_ERROR_ "$work/serial-1.log" "$work/serial-2.log"; then
  grep -a A1C_ERROR_ "$work"/serial-*.log >&2
  fail 'guest EFI assertion failed'
fi

passed=1
echo 'PASS: EFI bcfg/BLS/ACPI commands and volatile vs nonvolatile EFI variables'
