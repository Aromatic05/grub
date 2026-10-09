#!/bin/sh
# Isolated EFI integration test for the native "ventoy IMAGE" entry point.
# The Ventoy test disk is always mounted read-only; only temporary ESP
# and OVMF variable images may be modified.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${VENTOY_BUILD:-"$repo/build-x86_64-efi"}
test_disk=${VENTOY_TEST_DISK:-"$repo/../out/testenv/images/ventoy-test.raw"}
iso_name=${VENTOY_TEST_ISO:-archlinux-2026.03.01-x86_64.iso}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
mode=${1:---check}

case "$mode" in
  --check|--boot|--grubfm) ;;
  *) echo "Usage: $0 [--check|--boot|--grubfm]" >&2; exit 2 ;;
esac
for tool in mkfs.vfat mmd mcopy qemu-system-x86_64 timeout grep sed; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "Missing: $tool" >&2
    exit 1
  }
done
for path in "$build/grub-mkstandalone" "$build/grub-core/ventoy.mod" \
            "$test_disk" "$ovmf_code" "$ovmf_vars"; do
  test -f "$path" || { echo "Missing file: $path" >&2; exit 1; }
done

test_work=$(mktemp -d "${TMPDIR:-/tmp}/ventoy-boot.XXXXXX")
pass=0
cleanup()
{
  if test "$pass" = 1 && test "${VENTOY_TEST_KEEP:-0}" != 1; then rm -rf "$test_work"
  else echo "Ventoy test output kept: $test_work" >&2; fi
}
trap cleanup EXIT HUP INT TERM

if test "$mode" = "--check"; then
  cat > "$test_work/grub.cfg" <<'GRUBCFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set debug=
echo VTOY_TEST_BEGIN
insmod ventoy
if ventoy --check (hd1,msdos1)/ISO/@ARCH_ISO@; then
  echo VTOY_CHECK_AUTODISCOVER_OK
else
  echo VTOY_ERROR_AUTODISCOVER
fi
if ventoy --runtime=(hd1,msdos2)/ventoy --check (hd1,msdos1)/ISO/@ARCH_ISO@; then
  echo VTOY_CHECK_EXPLICIT_OK
else
  echo VTOY_ERROR_EXPLICIT
fi
if ventoy --check (hd1,msdos1)/ISO/missing.iso; then
  echo VTOY_ERROR_MISSING_ACCEPTED
else
  echo VTOY_CHECK_MISSING_REJECTED
fi
if ventoy --check (hd1,msdos1)/ISO/bad.txt; then
  echo VTOY_ERROR_TYPE_ACCEPTED
else
  echo VTOY_CHECK_TYPE_REJECTED
fi
if ventoy --runtime=(hd1,msdos2)/absent --check (hd1,msdos1)/ISO/@ARCH_ISO@; then
  echo VTOY_ERROR_RUNTIME_ACCEPTED
else
  echo VTOY_CHECK_RUNTIME_REJECTED
fi
echo VTOY_TEST_DONE
halt
GRUBCFG
elif test "$mode" = "--grubfm"; then
  cat > "$test_work/grub.cfg" <<'GRUBCFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set vtoy_diag=on
set debug=chain,ventoyboot
echo VTOY_GRUBFM_BEGIN
insmod grubfm
grubfm_set -r hd0
grubfm_open (hd1,msdos1)/ISO/@ARCH_ISO@
if [ "$grubfm_file" = "(hd1,msdos1)/ISO/@ARCH_ISO@" ]; then
  echo VTOY_GRUBFM_CONTEXT_OK
else
  echo VTOY_ERROR_GRUBFM_CONTEXT
fi
ini_get --set=vt_rule (hd0)/boot/grubfm/types/iso 0:menu
if [ "$vt_rule" = "ventoy.cfg" ]; then
  echo VTOY_GRUBFM_RULE_OK
else
  echo VTOY_ERROR_GRUBFM_RULE
fi
configfile (hd0)/boot/grubfm/rules/ventoy.cfg
echo VTOY_BOOT_RETURNED
halt
GRUBCFG
else
  cat > "$test_work/grub.cfg" <<'GRUBCFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set vtoy_diag=on
set debug=chain,ventoydbg,ventoyboot
echo VTOY_BOOT_LINUX_BEGIN
insmod ventoy
ventoy (hd1,msdos1)/ISO/@ARCH_ISO@
echo VTOY_BOOT_RETURNED
halt
GRUBCFG
fi
sed -i "s/@ARCH_ISO@/$iso_name/g" "$test_work/grub.cfg"

"$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$test_work/BOOTX64.EFI" --fonts='' --locales='' --compress=no \
  --modules='serial terminal normal echo halt loopback chain boot ventoy part_msdos fat iso9660 udf grubfm' \
  "boot/grub/grub.cfg=$test_work/grub.cfg" > "$test_work/pack.log" 2>&1

truncate -s 64M "$test_work/esp.img"
mkfs.vfat -F 16 -n VTOYTEST "$test_work/esp.img" >/dev/null
mmd -i "$test_work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$test_work/esp.img" "$test_work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
if test "$mode" = "--grubfm"; then
  mmd -i "$test_work/esp.img" ::/boot ::/boot/grubfm ::/boot/grubfm/types ::/boot/grubfm/rules
  mcopy -i "$test_work/esp.img" "$repo/extras/grubfm/boot/grubfm/types/iso" ::/boot/grubfm/types/iso
  mcopy -i "$test_work/esp.img" "$repo/extras/grubfm/boot/grubfm/rules/ventoy.cfg" ::/boot/grubfm/rules/ventoy.cfg
fi
cp "$ovmf_vars" "$test_work/OVMF_VARS.fd"

qemu_status=0
if test "$mode" != "--check" && test "${VENTOY_BOOT_CAPTURE:-0}" = 1; then
  command -v socat >/dev/null 2>&1 || {
    echo "socat required for VENTOY_BOOT_CAPTURE" >&2; exit 1;
  }
  qemu_timeout=105s
  monitor="unix:$test_work/monitor.sock,server,nowait"
else
  qemu_timeout=60s
  monitor=none
fi
timeout "$qemu_timeout" qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 -m "${VENTOY_QEMU_MEM:-3072M}" \
  -display none -monitor "$monitor" -no-reboot \
  -serial "file:$test_work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$test_work/OVMF_VARS.fd" \
  -drive "if=virtio,format=raw,readonly=on,file=$test_work/esp.img" \
  -drive "if=virtio,format=raw,readonly=on,file=$test_disk" \
  >"$test_work/qemu.log" 2>&1 &
qemu_pid=$!

if test "$mode" != "--check" && test "${VENTOY_BOOT_CAPTURE:-0}" = 1; then
  sleep 27
  if test -S "$test_work/monitor.sock"; then
    printf 'screendump %s/menu.ppm\n' "$test_work" |
      socat -T 2 - "UNIX-CONNECT:$test_work/monitor.sock" > "$test_work/monitor.log" 2>&1 || true
    printf 'sendkey ret\n' |
      socat -T 2 - "UNIX-CONNECT:$test_work/monitor.sock" >> "$test_work/monitor.log" 2>&1 || true
    sleep 35
    printf 'screendump %s/after-enter.ppm\n' "$test_work" |
      socat -T 2 - "UNIX-CONNECT:$test_work/monitor.sock" >> "$test_work/monitor.log" 2>&1 || true
    sleep 27
    if test -S "$test_work/monitor.sock"; then
      printf 'screendump %s/later.ppm\n' "$test_work" |
        socat -T 2 - "UNIX-CONNECT:$test_work/monitor.sock" >> "$test_work/monitor.log" 2>&1 || true
    fi
    for shot in menu after-enter later; do
      if test -s "$test_work/$shot.ppm"; then
        magick "$test_work/$shot.ppm" "$test_work/$shot.png" || true
        echo "Frame capture: $test_work/$shot.png"
      fi
    done
  fi
fi
wait "$qemu_pid" || qemu_status=$?
cat "$test_work/qemu.log"
if test "$qemu_status" -ne 0 && test "$qemu_status" -ne 124; then
  echo "QEMU exited with $qemu_status" >&2
  exit 1
fi
if test "$mode" = "--check"; then start_marker=VTOY_TEST_BEGIN
elif test "$mode" = "--grubfm"; then start_marker=VTOY_GRUBFM_BEGIN
else start_marker=VTOY_BOOT_LINUX_BEGIN; fi
grep -q "$start_marker" "$test_work/serial.log" || {
  echo "Boot did not reach test script" >&2
  tail -n 30 "$test_work/serial.log" >&2
  exit 1
}
if test "$mode" = "--check"; then
  for marker in AUTODISCOVER_OK EXPLICIT_OK MISSING_REJECTED TYPE_REJECTED RUNTIME_REJECTED; do
    grep -q "VTOY_CHECK_$marker" "$test_work/serial.log" || {
      echo "Check failed: $marker" >&2
      tail -n 55 "$test_work/serial.log" >&2
      exit 1
    }
  done
  grep -q VTOY_TEST_DONE "$test_work/serial.log"
  if grep -q VTOY_ERROR_ "$test_work/serial.log"; then
    echo "Unexpected Ventoy check acceptance" >&2
    exit 1
  fi
  echo "PASS: native Ventoy command, automatic/explicit runtime discovery, rejection"
else
  if test "$mode" = "--grubfm"; then
    grep -aq VTOY_GRUBFM_CONTEXT_OK "$test_work/serial.log" || {
      echo "FAIL: GRUBFM file selection context missing" >&2; exit 1;
    }
    grep -aq VTOY_GRUBFM_RULE_OK "$test_work/serial.log" || {
      echo "FAIL: GRUBFM INI ISO action missing" >&2; exit 1;
    }
  fi
  grep -aq 'Arch Linux install medium' "$test_work/serial.log" || {
    echo "FAIL: Arch Linux EFI boot menu was not reached" >&2
    tail -n 50 "$test_work/serial.log" >&2
    exit 1
  }
  # GRUBFM's configfile executes in a separate GRUB environment scope;
  # vtoy_diag need not be exported from the test script. The actual
  # Arch EFI menu check below validates the GRUBFM execution path.
  if test "$mode" = "--boot"; then
    grep -q '\[VTOY_DIAG\] linux_chain done:' "$test_work/serial.log" || {
      echo "Linux chain data construction did not finish" >&2
      tail -n 70 "$test_work/serial.log" >&2
      exit 1
    }
  fi
  echo "EFI handoff markers:"
  grep -aE 'VTOY_BOOT_|\[VTOY_DIAG\]|chainloader|StartImage|LoadImage|ventoyboot' "$test_work/serial.log" | tail -n 35 | cut -c1-220 || true
  if grep -q VTOY_BOOT_RETURNED "$test_work/serial.log"; then
    echo "FAIL: EFI consumer returned to GRUB" >&2
    exit 1
  fi
  echo "PASS: $mode reached Arch Linux EFI boot menu without returning (guest login verified separately)"
fi
pass=1
