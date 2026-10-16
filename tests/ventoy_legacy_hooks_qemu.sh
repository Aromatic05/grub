#!/bin/sh
# Minimal OVMF regression for module-owned Ventoy compatibility hooks.
# Uses a disposable ext2 disk and ESP; no host devices are written.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${VENTOY_BUILD:-"$repo/build-x86_64-efi"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}

for tool in mkfs.vfat mkfs.ext2 mmd mcopy qemu-system-x86_64 sha256sum timeout grep; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "Missing $tool" >&2
    exit 1
  }
done
for file in "$build/grub-mkstandalone" "$build/grub-core/ventoy.mod" \
            "$build/grub-core/moddep.lst" "$ovmf_code" "$ovmf_vars"; do
  test -f "$file" || { echo "Missing $file" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/ventoy-hooks.XXXXXX")
ok=0
finish()
{
  if test "$ok" = 1 && test "${VENTOY_TEST_KEEP:-0}" != 1; then
    rm -rf "$work"
  else
    echo "Ventoy hook evidence: $work" >&2
  fi
}
trap finish EXIT HUP INT TERM

mkdir -p "$work/root"
printf 'Ventoy legacy FSHash regression\n' > "$work/root/MiXeD.txt"
expected=$(sha256sum "$work/root/MiXeD.txt" | cut -d ' ' -f 1)
truncate -s 32M "$work/files.img"
mkfs.ext2 -q -F -d "$work/root" "$work/files.img"

cat > "$work/grub.cfg" <<'GRUBCFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set prefix=(hd0)/boot/grub
echo VTOY_HOOK_BEGIN
if insmod ventoy; then echo VTOY_HOOK_LOAD_OK; else echo VTOY_HOOK_LOAD_FAIL; fi
if sha256sum (hd1)/mixed.txt; then echo VTOY_HOOK_WRONG_DEFAULT; else echo VTOY_HOOK_DEFAULT_OK; fi
vt_fs_ignore_case 1
if sha256sum (hd1)/mixed.txt; then echo VTOY_HOOK_CASE_OK; else echo VTOY_HOOK_CASE_FAIL; fi
if [ "$VT_LAST_CHECK_SUM" = "@EXPECTED@" ]; then echo VTOY_HOOK_HASH_OK; else echo VTOY_HOOK_HASH_FAIL; fi
vt_fs_ignore_case 0
if sha256sum (hd1)/mixed.txt; then echo VTOY_HOOK_WRONG_RESET; else echo VTOY_HOOK_RESET_OK; fi
if vt_sbinfo; then echo VTOY_HOOK_SBINFO_OK; else echo VTOY_HOOK_SBINFO_FAIL; fi
if vt_update_sb_policy; then echo VTOY_HOOK_SBPOLICY_OK; else echo VTOY_HOOK_SBPOLICY_FAIL; fi
if vt_set_boot_opt rdinit=/vtoy/vtoy; then echo VTOY_HOOK_SET_OPT_OK; else echo VTOY_HOOK_SET_OPT_FAIL; fi
vt_unset_boot_opt
vt_img_extra_initrd_reset
rmmod ventoy
unset VT_LAST_CHECK_SUM
sha256sum (hd1)/MiXeD.txt
if [ -z "$VT_LAST_CHECK_SUM" ]; then echo VTOY_HOOK_UNLOAD_OK; else echo VTOY_HOOK_UNLOAD_FAIL; fi
if insmod ventoy; then echo VTOY_HOOK_RELOAD_OK; else echo VTOY_HOOK_RELOAD_FAIL; fi
rmmod ventoy
echo VTOY_HOOK_DONE
halt
GRUBCFG
sed -i "s/@EXPECTED@/$expected/g" "$work/grub.cfg"

"$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --compress=no \
  --modules='serial terminal normal echo halt minicmd test part_msdos fat ext2 btrfs fshelp hashsum gcry_sha256 gcry_sha1 gcry_md5 crypto datetime elf extcmd font iso9660 mmap udf video wimboot boot relocator linux' \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1

truncate -s 48M "$work/esp.img"
mkfs.vfat -F 16 -n VTOYHOOK "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT ::/boot ::/boot/grub ::/boot/grub/x86_64-efi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$work/esp.img" "$build/grub-core/ventoy.mod" ::/boot/grub/x86_64-efi/ventoy.mod
mcopy -i "$work/esp.img" "$build/grub-core/moddep.lst" ::/boot/grub/x86_64-efi/moddep.lst
cp "$ovmf_vars" "$work/OVMF_VARS.fd"

status=0
timeout 60s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 1024M -display none -monitor none -no-reboot \
  -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/OVMF_VARS.fd" \
  -drive "if=virtio,format=raw,readonly=on,file=$work/esp.img" \
  -drive "if=virtio,format=raw,readonly=on,file=$work/files.img" \
  > "$work/qemu.log" 2>&1 || status=$?

test "$status" = 0 || { echo "QEMU exited $status" >&2; exit 1; }
for marker in VTOY_HOOK_BEGIN VTOY_HOOK_LOAD_OK VTOY_HOOK_DEFAULT_OK \
              VTOY_HOOK_CASE_OK VTOY_HOOK_HASH_OK VTOY_HOOK_RESET_OK \
              VTOY_HOOK_SBINFO_OK VTOY_HOOK_SBPOLICY_OK VTOY_HOOK_SET_OPT_OK \
              VTOY_HOOK_UNLOAD_OK VTOY_HOOK_RELOAD_OK VTOY_HOOK_DONE; do
  grep -aq "$marker" "$work/serial.log" || {
    echo "Missing guest assertion: $marker" >&2
    exit 1
  }
done
if grep -aq 'VTOY_HOOK_.*FAIL\|VTOY_HOOK_WRONG\|#PF\|can.t find command' "$work/serial.log"; then
  echo 'Unexpected guest failure' >&2
  exit 1
fi
echo "PASS: Ventoy FS casefold, SHA digest, Shim fallback, Linux CLI and module reload"
ok=1
