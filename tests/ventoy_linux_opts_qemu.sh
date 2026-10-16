#!/bin/sh
# OVMF regression for the Linux argument and initrd hooks supplied by Ventoy.
# The historical Ventoy disk image is always opened read-only.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${VENTOY_BUILD:-"$repo/build-x86_64-efi"}
test_disk=${VENTOY_TEST_DISK:-"$repo/../out/testenv/images/ventoy-test.raw"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
iso=${VENTOY_TEST_ISO:-archlinux-2026.03.01-x86_64.iso}

for cmd in mkfs.vfat mmd mcopy qemu-system-x86_64 timeout python3 grep; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "Missing $cmd" >&2; exit 1; }
done
for f in "$build/grub-mkstandalone" "$build/grub-core/ventoy.mod" \
         "$build/grub-core/moddep.lst" "$test_disk" "$ovmf_code" "$ovmf_vars"; do
  test -f "$f" || { echo "Missing $f" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/ventoy-linuxopts.XXXXXX")
ok=0
qpid=
cleanup()
{
  if test -n "$qpid"; then
    kill "$qpid" 2>/dev/null || :
    wait "$qpid" 2>/dev/null || :
  fi
  if test "$ok" = 1 && test "${VENTOY_TEST_KEEP:-0}" != 1; then
    rm -rf "$work"
  else
    echo "Ventoy Linux loader evidence: $work" >&2
  fi
}
trap cleanup EXIT HUP INT TERM

printf 'synthetic initrd fixture\n' > "$work/tiny.cpio"
printf 'synthetic .ko fixture\n' > "$work/dm-mod.ko"
cat > "$work/explicit.cfg" <<'GRUBCFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set debug=ventoyopts
set pager=0
set prefix=(hd0)/boot/grub
echo VTOY_LINUX_BEGIN
if insmod ventoy; then echo VTOY_LINUX_MODULE_OK; else echo VTOY_LINUX_FAIL_MODULE; fi
if loopback iso (hd1,msdos1)/ISO/@ISO@; then echo VTOY_LINUX_LOOPBACK_OK; else echo VTOY_LINUX_FAIL_LOOPBACK; fi
set kparams='example=123 init=/original quiet'
set vtoy_img_part_file=(hd0)/tiny.cpio
if vt_img_extra_initrd_append (hd0)/drivers/dm-mod; then echo VTOY_LINUX_EXTRA_OK; else echo VTOY_LINUX_FAIL_EXTRA; fi
if vt_set_boot_opt '@kparams' rdinit=/vtoy/vtoy init=/override; then echo VTOY_LINUX_OPTIONS_OK; else echo VTOY_LINUX_FAIL_OPTIONS; fi
if linux (iso)/arch/boot/x86_64/vmlinuz-linux console=ttyS0 quiet noinitrd vga=current dm=/dev/xyz -- custom=tail; then echo VTOY_LINUX_KERNEL_OK; else echo VTOY_LINUX_FAIL_KERNEL; fi
if initrd (hd0)/tiny.cpio; then echo VTOY_LINUX_INITRD_OK; else echo VTOY_LINUX_FAIL_INITRD; fi
vt_unset_boot_opt
vt_img_extra_initrd_reset
unset vtoy_img_part_file
if linux (iso)/arch/boot/x86_64/vmlinuz-linux console=ttyS0 normal=ok; then echo VTOY_LINUX_CLEAN_KERNEL_OK; else echo VTOY_LINUX_FAIL_CLEAN_KERNEL; fi
if initrd (hd0)/tiny.cpio; then echo VTOY_LINUX_CLEAN_INITRD_OK; else echo VTOY_LINUX_FAIL_CLEAN_INITRD; fi
echo VTOY_LINUX_DONE
halt
GRUBCFG

cat > "$work/auto.cfg" <<'GRUBCFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set debug=ventoyopts
set pager=0
set prefix=(hd0)/boot/grub
echo VTOY_AUTO_BEGIN
insmod ventoy
loopback iso (hd1,msdos1)/ISO/@ISO@
set vtoy_img_part_file=(hd0)/tiny.cpio
vt_set_boot_opt rdinit=/vtoy/vtoy example=auto
if linux (iso)/arch/boot/x86_64/vmlinuz-linux console=ttyS0 loglevel=7; then echo VTOY_AUTO_KERNEL_OK; else echo VTOY_AUTO_KERNEL_FAIL; fi
echo VTOY_AUTO_BEFORE_BOOT
boot
echo VTOY_AUTO_UNEXPECTED_RETURN
halt
GRUBCFG
sed -i "s/@ISO@/$iso/g" "$work/explicit.cfg" "$work/auto.cfg"

truncate -s 48M "$work/esp.img"
mkfs.vfat -F 16 -n VTOYLINUX "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT ::/boot ::/boot/grub ::/boot/grub/x86_64-efi ::/drivers
mcopy -i "$work/esp.img" "$work/tiny.cpio" ::/tiny.cpio
mcopy -i "$work/esp.img" "$work/dm-mod.ko" ::/drivers/dm-mod.ko
mcopy -i "$work/esp.img" "$build/grub-core/ventoy.mod" ::/boot/grub/x86_64-efi/ventoy.mod
mcopy -i "$work/esp.img" "$build/grub-core/moddep.lst" ::/boot/grub/x86_64-efi/moddep.lst

deps='serial terminal normal echo halt minicmd test part_msdos fat ext2 fshelp hashsum gcry_sha256 crypto extcmd btrfs datetime elf font gcry_md5 gcry_sha1 iso9660 mmap udf video wimboot linux relocator boot loopback'
for cfg in explicit auto; do
  "$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
    -o "$work/$cfg.EFI" --fonts='' --locales='' --compress=no \
    --modules="$deps" "boot/grub/grub.cfg=$work/$cfg.cfg" \
    > "$work/pack-$cfg.log" 2>&1
done

mcopy -i "$work/esp.img" "$work/explicit.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$ovmf_vars" "$work/OVMF_VARS.fd"
timeout 70s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 3072M -display none -monitor none -no-reboot \
  -serial "file:$work/explicit-serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/OVMF_VARS.fd" \
  -drive "if=virtio,format=raw,readonly=on,file=$work/esp.img" \
  -drive "if=virtio,format=raw,readonly=on,file=$test_disk" \
  > "$work/explicit-qemu.log" 2>&1
for marker in VTOY_LINUX_MODULE_OK VTOY_LINUX_LOOPBACK_OK VTOY_LINUX_EXTRA_OK \
              VTOY_LINUX_OPTIONS_OK VTOY_LINUX_KERNEL_OK VTOY_LINUX_INITRD_OK \
              VTOY_LINUX_CLEAN_KERNEL_OK VTOY_LINUX_CLEAN_INITRD_OK VTOY_LINUX_DONE; do
  grep -aq "$marker" "$work/explicit-serial.log" || {
    echo "Missing explicit kernel/initrd guest assertion: $marker" >&2
    exit 1
  }
done
if grep -aq 'VTOY_LINUX_FAIL\|#PF\|can.t find command' "$work/explicit-serial.log"; then
  echo "Unexpected explicit Linux guest failure" >&2
  exit 1
fi

# GRUB's debug prefix occupies most of a serial line. Join visual wraps
# before comparing the complete argv that reached the actual Linux loader.
python3 - "$work/explicit-serial.log" <<'PY'
import re
import sys

with open(sys.argv[1], "r", errors="replace", newline="") as f:
    lines = f.read().replace("\r", "").splitlines()
args = {}
current = None
for line in lines:
    m = re.search(r"ventoyopts: (kernel|initrd)\[(\d+)\]=(.*)", line)
    if m:
        current = (m.group(1), int(m.group(2)))
        args[current] = m.group(3)
    elif current is not None:
        if not line or line.startswith(("ventoy/", "VTOY_", "error:", "\x1b")):
            current = None
        else:
            args[current] += line

kernel = [
    "(iso)/arch/boot/x86_64/vmlinuz-linux",
    "console=ttyS0", "quiet", "DM=/dev/xyz", "example=123",
    "vtit=/original", "quiet", "rdinit=/vtoy/vtoy",
    "vtit=/override", "--", "custom=tail",
]
initrd = [
    "(hd0)/tiny.cpio",
    "newc:dm-mod.ko:(hd0)/drivers/dm-mod.ko",
    "(hd0)/tiny.cpio",
]
for kind, expected in (("kernel", kernel), ("initrd", initrd)):
    got = [args.get((kind, i)) for i in range(len(expected))]
    if got != expected:
        raise SystemExit(f"{kind} mismatch: got={got!r}; expected={expected!r}")
print("PASS: exact GRUB Linux cmdline order, filters, @env expansion and initrd vector")
PY

# Boot a real Arch Linux EFI kernel without explicitly executing initrd.
# It is expected to reach the kernel and panic on absent root filesystem;
# it is NOT an OS root filesystem or full Ventoy distro boot acceptance.
mcopy -o -i "$work/esp.img" "$work/auto.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$ovmf_vars" "$work/OVMF_VARS.fd"
: > "$work/auto-serial.log"
qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 3072M -display none -monitor none -no-reboot \
  -serial "file:$work/auto-serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/OVMF_VARS.fd" \
  -drive "if=virtio,format=raw,readonly=on,file=$work/esp.img" \
  -drive "if=virtio,format=raw,readonly=on,file=$test_disk" \
  > "$work/auto-qemu.log" 2>&1 &
qpid=$!
i=0
while test "$i" -lt 60; do
  if grep -aq 'Kernel panic - not syncing: VFS:' "$work/auto-serial.log" \
     && grep -aq 'example=auto' "$work/auto-serial.log"; then
    break
  fi
  if ! kill -0 "$qpid" 2>/dev/null; then break; fi
  sleep 1
  i=$((i + 1))
done
kill "$qpid" 2>/dev/null || :
wait "$qpid" 2>/dev/null || :
qpid=
for marker in VTOY_AUTO_KERNEL_OK VTOY_AUTO_BEFORE_BOOT preboot-initrd \
              'Linux version' 'example=auto'; do
  grep -aq "$marker" "$work/auto-serial.log" || {
    echo "Missing automatic initrd/kernel guest evidence: $marker" >&2
    exit 1
  }
done
if grep -aq 'VTOY_AUTO_KERNEL_FAIL\|VTOY_AUTO_UNEXPECTED_RETURN\|#PF' "$work/auto-serial.log"; then
  echo "Unexpected automatic initrd/kernel guest failure" >&2
  exit 1
fi
echo "PASS: Linux kernel entered, automatic preboot initrd executed (no rootfs expected)"
ok=1
