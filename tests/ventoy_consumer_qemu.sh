#!/bin/sh
# Replays the pre-unification WinPE EFI consumer path with the CURRENT ventoy.mod.
# All attached source disks are read-only. The historical Consumer is version-locked.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${VENTOY_BUILD:-"$repo/build-x86_64-efi"}
disk=${VENTOY_TEST_DISK:-"$repo/../out/testenv/images/ventoy-test.raw"}
runtime=${VENTOY_CONSUMER_RUNTIME:-"$repo/../out/testenv/work/ventoy-runtime-p1"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
mode=${1:---prepare}
case "$mode" in
  --prepare|--boot|--repro|--trace) ;;
  *) echo "Usage: $0 [--prepare|--boot|--repro|--trace]" >&2; exit 2 ;;
esac
for command in sha256sum mkfs.vfat mmd mcopy qemu-system-x86_64 timeout grep; do
  command -v "$command" >/dev/null 2>&1 || { echo "Missing command: $command" >&2; exit 1; }
done
for file in "$build/grub-mkstandalone" "$build/grub-core/ventoy.mod" "$disk"             "$ovmf_code" "$ovmf_vars" "$runtime/ventoy_x64.efi"             "$runtime/udf_x64.efi" "$runtime/iso9660_x64.efi"; do
  test -f "$file" || { echo "Missing file: $file" >&2; exit 1; }
done

# Historical image is not interchangeable with the stock Ventoy executable.
# Set VENTOY_CONSUMER_* explicitly to test another reviewed binary set.
expected_consumer=${VENTOY_CONSUMER_SHA256:-362612542b480fb1417e6c7fc498bc42214b8266d237e319f116c0f7a3f00538}
expected_udf=${VENTOY_UDF_SHA256:-64b39735f98129451373ff36b281cd93f678f96dc173eb4dea78104a161622c4}
expected_iso=${VENTOY_ISO9660_SHA256:-e3412168c2750fc81ca3a546261cf7cbfbf8b7b0212ccd6e8a7b26a40c1f8e22}
{
  printf '%s  %s\n' "$expected_consumer" "$runtime/ventoy_x64.efi"
  printf '%s  %s\n' "$expected_udf" "$runtime/udf_x64.efi"
  printf '%s  %s\n' "$expected_iso" "$runtime/iso9660_x64.efi"
} | sha256sum -c --status || {
  echo "Consumer/ISO/UDF driver mismatch; refused to mix unverified binaries" >&2
  exit 1
}

mkdir -p "${VENTOY_RUN_ROOT:-${TMPDIR:-/tmp}}"
work=$(mktemp -d "${VENTOY_RUN_ROOT:-${TMPDIR:-/tmp}}/ventoy-winpe.XXXXXX")
pass=0
cleanup()
{
  if test "$pass" = 1 && test "${VENTOY_TEST_KEEP:-0}" != 1; then
    rm -rf "$work"
  else
    echo "WinPE EFI consumer diagnostic retained: $work" >&2
  fi
}
trap cleanup EXIT HUP INT TERM
printf 'Mode: %s\nRuntime: %s\nConsumer-SHA256: %s\nUDF-SHA256: %s\nSource-disk: %s\n'   "$mode" "$runtime" "$expected_consumer" "$expected_udf" "$disk" >"$work/manifest.txt"
{
cat <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set debug=chain,vtchunkdbg
set ventoy_compatible=NO
set LoadIsoEfiDriver=on
set VTOY_CHKDEV_RESULT_STRING=0
echo WINPE_TEST_BEGIN
insmod ventoy
vt_windows_reset
if vt_img_sector (hd1,msdos1)/ISO/winpe.iso; then
  echo WINPE_EXTENT_OK
else
  echo WINPE_ERROR_EXTENT
  halt
fi
if loopback loop (hd1,msdos1)/ISO/winpe.iso; then
  echo WINPE_LOOPBACK_OK
else
  echo WINPE_ERROR_LOOPBACK
  halt
fi
vt_windows_collect_wim_patch bcd (loop)/EFI/MICROSOFT/BOOT/BCD
vt_windows_count_wim_patch vt_wim_count
echo WINPE_WIM_PATCH_COUNT=$vt_wim_count
vt_windows_locate_wim_patch (loop) /ISO/winpe.iso
vt_dump_wim_patch
loopback -d loop
if vt_windows_chain_data (hd1,msdos1)/ISO/winpe.iso; then
  echo WINPE_CHAIN_READY
else
  echo WINPE_ERROR_CHAIN
  halt
fi
echo WINPE_CHAIN_ADDR=$vtoy_chain_mem_addr
echo WINPE_CHAIN_SIZE=$vtoy_chain_mem_size
echo WINPE_ENV_PARAM=$env_param
GRUB
if test "$mode" != "--prepare"; then
  cat <<'GRUB'
if [ -z "$vtoy_chain_mem_addr" -o -z "$vtoy_chain_mem_size" -o -z "$env_param" ]; then
  echo WINPE_ERROR_NO_CHAIN
  halt
fi
vt_acpi_param $vtoy_chain_mem_addr 2048
echo WINPE_CONSUMER_BEGIN
chainloader (hd0)/ventoy/ventoy_x64.efi @DEBUG@ env_param=$env_param isoefi=on iso_udf FirstTry=@EFI@BOOT@bootx64.efi mem:$vtoy_chain_mem_addr:size:$vtoy_chain_mem_size
boot
echo WINPE_CONSUMER_RETURNED
GRUB
fi
cat <<'GRUB'
echo WINPE_TEST_END
halt
GRUB
} > "$work/grub.cfg"
if test "$mode" = "--trace"; then
  sed -i 's/@DEBUG@/debug/g' "$work/grub.cfg"
else
  sed -i 's/@DEBUG@//g' "$work/grub.cfg"
fi

"$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --compress=no \
  --modules='serial terminal normal echo halt loopback chain boot acpi ventoy part_msdos fat iso9660 udf'   "boot/grub/grub.cfg=$work/grub.cfg" >"$work/pack.log" 2>&1
truncate -s 64M "$work/esp.img"
mkfs.vfat -F 16 -n VTOYDIAG "$work/esp.img" >"$work/mkfs.log" 2>&1
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT ::/ventoy
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$work/esp.img" "$runtime/ventoy_x64.efi" ::/ventoy/ventoy_x64.efi
mcopy -i "$work/esp.img" "$runtime/udf_x64.efi" ::/ventoy/udf_x64.efi
mcopy -i "$work/esp.img" "$runtime/iso9660_x64.efi" ::/ventoy/iso9660_x64.efi
cp "$ovmf_vars" "$work/OVMF_VARS.fd"

status=0
monitor=none
if test "$mode" = "--trace"; then
  command -v socat >/dev/null 2>&1 || exit 1
  monitor="unix:$work/monitor.sock,server,nowait"
fi
timeout "${VENTOY_WINPE_TIMEOUT:-90s}" qemu-system-x86_64 -machine q35,accel=tcg \
  -cpu qemu64 -m "${VENTOY_QEMU_MEM:-3072M}" -display none -monitor "$monitor" -no-reboot \
  -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/OVMF_VARS.fd" \
  -drive "if=virtio,format=raw,readonly=on,file=$work/esp.img" \
  -drive "if=virtio,format=raw,readonly=on,file=$disk"   >"$work/qemu.log" 2>&1 &
pid=$!
if test "$mode" = "--trace"; then
  attempts=0
  while test ! -S "$work/monitor.sock" && test "$attempts" -lt 30; do
    sleep 0.2
    attempts=$((attempts + 1))
  done
  if test -S "$work/monitor.sock"; then
    {
      sleep 3
      count=0
      while test "$count" -lt 175 && kill -0 "$pid" 2>/dev/null; do
        printf 'sendkey ret\n'
        sleep 0.35
        count=$((count + 1))
      done
    } | socat -T 2 - "UNIX-CONNECT:$work/monitor.sock" >"$work/monitor.log" 2>&1 || true
  fi
fi
wait "$pid" || status=$?

grep -aq WINPE_TEST_BEGIN "$work/serial.log" || {
  echo "FAIL: GRUB test script never started" >&2; exit 1;
}
for marker in WINPE_EXTENT_OK WINPE_LOOPBACK_OK WINPE_CHAIN_READY WINPE_WIM_PATCH_COUNT=1; do
  grep -aq "$marker" "$work/serial.log" || {
    echo "FAIL: $marker absent; see $work/serial.log" >&2; exit 1;
  }
done
if grep -aq WINPE_ERROR_ "$work/serial.log"; then
  echo "FAIL: GRUB reported an error; see $work/serial.log" >&2; exit 1
fi
printf 'EFI consumer milestones:\n'
grep -aE 'WINPE_|StartImage|LoadImage|chain:|Error:|error:' "$work/serial.log" | tail -n 60 | cut -c1-240 || true
if test "$mode" = "--prepare"; then
  echo "PASS: current ventoy.mod produces WinPE chain without executing Consumer"
  pass=1
elif test "$mode" = "--repro"; then
  if ! grep -aq WINPE_CONSUMER_RETURNED "$work/serial.log" ||
     ! grep -aEq 'StartImage status=0x0' "$work/serial.log"; then
    echo "NOT REPRODUCED: expected historical masked EFI_SUCCESS failure" >&2
    exit 1
  fi
  echo "PASS (known bug reproduced): WinPE did not boot, but Consumer returned EFI_SUCCESS"
  pass=1
else
  if grep -aq WINPE_CONSUMER_RETURNED "$work/serial.log"; then
    echo "FAIL: EFI Consumer returned to GRUB, inspect StartImage and EFI protocol trace" >&2
    exit 1
  fi
  echo "UNVERIFIED: Consumer did not return; Windows desktop has NOT been asserted (QEMU=$status)" >&2
  exit 2
fi
