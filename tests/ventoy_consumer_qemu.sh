#!/bin/sh
# Diagnoses the current WinPE chain against the known working Ventoy handoff.
# All original RAW fixture disks remain unchanged; IDE guests use disposable COW.
# The historical Consumer and jump executable are version-locked.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${VENTOY_BUILD:-"$repo/build-x86_64-efi"}
disk=${VENTOY_TEST_DISK:-"$repo/../out/testenv/images/ventoy-test.raw"}
runtime=${VENTOY_CONSUMER_RUNTIME:-"$repo/../out/testenv/work/ventoy-runtime-p1"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
mode=${1:---prepare}
consumer_fs=${VENTOY_CONSUMER_FS:-udf}
# The historical Windows guest uses SATA/IDE disks. A virtio boot disk may
# work in OVMF yet be invisible in Windows PE without a guest virtio driver.
guest_bus=${VENTOY_GUEST_DISK_BUS:-ide}
case "$guest_bus" in
  ide|virtio) ;;
  *) echo "Unsupported VENTOY_GUEST_DISK_BUS: $guest_bus" >&2; exit 2 ;;
esac
case "$consumer_fs" in
  udf|fat) ;;
  *) echo "Unsupported VENTOY_CONSUMER_FS: $consumer_fs" >&2; exit 2 ;;
esac
case "$mode" in
  --prepare|--boot|--invalid-params|--trace|--direct-check|--direct-boot) ;;
  *) echo "Usage: $0 [--prepare|--boot|--invalid-params|--trace|--direct-check|--direct-boot]" >&2; exit 2 ;;
esac
for command in sha256sum mkfs.vfat mmd mcopy qemu-system-x86_64 timeout grep; do
  command -v "$command" >/dev/null 2>&1 || { echo "Missing command: $command" >&2; exit 1; }
done
for file in "$build/grub-mkstandalone" "$build/grub-core/ventoy.mod" "$disk"             "$ovmf_code" "$ovmf_vars" "$runtime/ventoy_x64.efi"              "$runtime/udf_x64.efi" "$runtime/iso9660_x64.efi" "$runtime/vtoyjump64.exe"; do
  test -f "$file" || { echo "Missing file: $file" >&2; exit 1; }
done

# Historical image is not interchangeable with the stock Ventoy executable.
# Set VENTOY_CONSUMER_* explicitly to test another reviewed binary set.
expected_consumer=${VENTOY_CONSUMER_SHA256:-362612542b480fb1417e6c7fc498bc42214b8266d237e319f116c0f7a3f00538}
expected_udf=${VENTOY_UDF_SHA256:-64b39735f98129451373ff36b281cd93f678f96dc173eb4dea78104a161622c4}
expected_iso=${VENTOY_ISO9660_SHA256:-e3412168c2750fc81ca3a546261cf7cbfbf8b7b0212ccd6e8a7b26a40c1f8e22}
expected_jump=${VENTOY_JUMP64_SHA256:-5787798ecc49c6a179bca0b6b07854b9f46a3f96b11731f8d1c4cc6fe8b5ecce}
{
  printf '%s  %s\n' "$expected_consumer" "$runtime/ventoy_x64.efi"
  printf '%s  %s\n' "$expected_udf" "$runtime/udf_x64.efi"
  printf '%s  %s\n' "$expected_iso" "$runtime/iso9660_x64.efi"
  printf '%s  %s\n' "$expected_jump" "$runtime/vtoyjump64.exe"
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
printf 'Consumer-FS: %s\n' "$consumer_fs" >>"$work/manifest.txt"
printf 'Guest-Disk-Bus: %s\n' "$guest_bus" >>"$work/manifest.txt"
{
if test "$mode" = "--direct-check"; then
  cat <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
echo WINPE_DIRECT_CHECK_BEGIN
insmod ventoy
if ventoy --check (hd1,msdos1)/ISO/winpe.iso; then
  echo WINPE_DIRECT_CHECK_OK
else
  echo WINPE_ERROR_DIRECT_CHECK
fi
halt
GRUB
elif test "$mode" = "--direct-boot"; then
  cat <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set debug=chain,vtchunkdbg,ventoyboot
echo WINPE_DIRECT_BOOT_BEGIN
insmod ventoy
ventoy (hd1,msdos1)/ISO/winpe.iso
echo WINPE_CONSUMER_RETURNED
halt
GRUB
else
cat <<'GRUB'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set debug=chain,vtchunkdbg
set ventoy_compatible=NO
unset LoadIsoEfiDriver
set VTOY_CHKDEV_RESULT_STRING=0
echo WINPE_TEST_BEGIN
insmod ventoy
set vtoy_path=(hd0)/ventoy
if [ ! -f "$vtoy_path/vtoyjump64.exe" ]; then
  echo WINPE_ERROR_JUMP_MISSING
  halt
fi
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
# The original Ventoy menu chooses this independent of the WIM file reader.
if vt_is_udf (hd1,msdos1)/ISO/winpe.iso; then
  set ventoy_fs_probe=udf
else
  set ventoy_fs_probe=iso9660
fi
echo WINPE_ISO_FS=$ventoy_fs_probe
vt_windows_collect_wim_patch bcd (loop)/EFI/MICROSOFT/BOOT/BCD
vt_windows_count_wim_patch vt_wim_count
echo WINPE_WIM_PATCH_COUNT=$vt_wim_count
vt_windows_locate_wim_patch (loop) /ISO/winpe.iso
vt_dump_wim_patch
loopback -d loop
if vt_windows_chain_data (hd1,msdos1)/ISO/winpe.iso; then
  echo WINPE_CHAIN_READY
  echo WINPE_AUTO_ISOEFI=$LoadIsoEfiDriver
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
chainloader (hd0)/ventoy/ventoy_x64.efi @DEBUG@ env_param=$env_param isoefi=@ISOEFI_VALUE@ @FS_OPTION@ @FIRSTTRY@ mem:$vtoy_chain_mem_addr:size:$vtoy_chain_mem_size
boot
echo WINPE_CONSUMER_RETURNED
GRUB
fi
cat <<'GRUB'
echo WINPE_TEST_END
halt
GRUB
fi
} > "$work/grub.cfg"
sed -i "s/@FS_OPTION@/iso_$consumer_fs/g" "$work/grub.cfg"
if test "${VENTOY_FS_DEBUG:-0}" = 1; then
  sed -i 's/set debug=chain,vtchunkdbg/set debug=chain,vtchunkdbg,portdbg/' "$work/grub.cfg"
fi
# --invalid-params deliberately replays our previously INCORRECT handoff. All other
# modes use the upstream Windows path: no forced isoefi driver or FirstTry.
if test "$mode" = "--invalid-params"; then
  sed -i 's/@ISOEFI_VALUE@/on/;s/@FIRSTTRY@/FirstTry=@EFI@BOOT@bootx64.efi/' "$work/grub.cfg"
else
  sed -i 's/@ISOEFI_VALUE@/$LoadIsoEfiDriver/;s/@FIRSTTRY@//' "$work/grub.cfg"
fi
if test "$mode" = "--trace"; then
  sed -i 's/@DEBUG@/debug/g' "$work/grub.cfg"
else
  sed -i 's/@DEBUG@//g' "$work/grub.cfg"
fi

modules='serial terminal normal echo halt loopback chain boot acpi ventoy part_msdos fat iso9660 udf'
"$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --compress=no \
  --modules="$modules"   "boot/grub/grub.cfg=$work/grub.cfg" >"$work/pack.log" 2>&1
truncate -s 64M "$work/esp.img"
mkfs.vfat -F 16 -n VTOYDIAG "$work/esp.img" >"$work/mkfs.log" 2>&1
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT ::/ventoy
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$work/esp.img" "$runtime/ventoy_x64.efi" ::/ventoy/ventoy_x64.efi
mcopy -i "$work/esp.img" "$runtime/udf_x64.efi" ::/ventoy/udf_x64.efi
mcopy -i "$work/esp.img" "$runtime/iso9660_x64.efi" ::/ventoy/iso9660_x64.efi
mcopy -i "$work/esp.img" "$runtime/vtoyjump64.exe" ::/ventoy/vtoyjump64.exe
cp "$ovmf_vars" "$work/OVMF_VARS.fd"

if test "$guest_bus" = "ide"; then
  # Writable AHCI frontend with disposable COW backing. All test fixture
  # source RAW images remain unchanged; Windows sees its native SATA driver.
  command -v qemu-img >/dev/null 2>&1 || { echo "Missing qemu-img" >&2; exit 1; }
  qemu-img create -q -f qcow2 -F raw -b "$work/esp.img" "$work/esp.qcow2"
  qemu-img create -q -f qcow2 -F raw -b "$disk" "$work/ventoy.qcow2"
  guest_esp_drive="if=ide,index=0,format=qcow2,file=$work/esp.qcow2"
  guest_source_drive="if=ide,index=1,format=qcow2,file=$work/ventoy.qcow2"
else
  # Negative control for WinPE installations without a VirtIO disk driver.
  guest_esp_drive="if=virtio,format=raw,readonly=on,file=$work/esp.img"
  guest_source_drive="if=virtio,format=raw,readonly=on,file=$disk"
fi

status=0
monitor=none
if test "$mode" = "--trace" || test "${VENTOY_SCREENSHOT:-0}" = "1"; then
  command -v socat >/dev/null 2>&1 || exit 1
  monitor="unix:$work/monitor.sock,server,nowait"
fi
timeout "${VENTOY_WINPE_TIMEOUT:-90s}" qemu-system-x86_64 -machine q35,accel=tcg \
  -cpu qemu64 -m "${VENTOY_QEMU_MEM:-3072M}" -display none -monitor "$monitor" -no-reboot \
  -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/OVMF_VARS.fd" \
  -drive "$guest_esp_drive" \
  -drive "$guest_source_drive" >"$work/qemu.log" 2>&1 &
pid=$!
if { test "$mode" = "--boot" || test "$mode" = "--direct-boot"; } &&
   test "${VENTOY_SCREENSHOT:-0}" = "1"; then
  sleep "${VENTOY_CAPTURE_AFTER:-120}"
  if test -S "$work/monitor.sock" && kill -0 "$pid" 2>/dev/null; then
    printf 'screendump %s/winpe-screen.ppm\n' "$work" |
      socat -T 3 - "UNIX-CONNECT:$work/monitor.sock" >"$work/screenshot.log" 2>&1 || true
    if test -s "$work/winpe-screen.ppm"; then
      if command -v magick >/dev/null 2>&1; then
        magick "$work/winpe-screen.ppm" "$work/winpe-screen.png" || true
      fi
      echo "WinPE screenshot for manual acceptance: $work/winpe-screen.png"
    fi
  fi
fi
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

if test "$mode" = "--direct-check"; then
  grep -aq WINPE_DIRECT_CHECK_OK "$work/serial.log" &&
  grep -aq 'Ventoy: Windows ISO readable;' "$work/serial.log" &&
  ! grep -aq WINPE_ERROR_DIRECT_CHECK "$work/serial.log" || {
    echo "FAIL: public ventoy --check did not accept the WinPE ISO" >&2; exit 1;
  }
  echo "PASS: public ventoy --check recognizes Windows ISO and finds runtime"
  pass=1
  exit 0
fi
if test "$mode" = "--direct-boot"; then
  grep -aq WINPE_DIRECT_BOOT_BEGIN "$work/serial.log" || {
    echo "FAIL: public ventoy PATH not reached" >&2; exit 1;
  }
  # GRUB's serial terminal wraps long debug lines; assert stable fragments.
  grep -aq 'Windows image' "$work/serial.log" &&
  grep -aq 'fs=udf WIM candidates=1' "$work/serial.log" || {
    echo "FAIL: direct Ventoy Windows/WIM pipeline did not run" >&2; exit 1;
  }
  for field in 'bytes=1667736' 'override=4' 'virt=460988416 udf=1'; do
    grep -aFq "$field" "$work/serial.log" || {
      echo "FAIL: public Ventoy WIM chain differs from verified fixture ($field)" >&2
      exit 1
    }
  done
  grep -aq 'EFI Windows' "$work/serial.log" &&
  grep -aq 'chainloader argc=5' "$work/serial.log" || {
    echo "FAIL: direct Ventoy did not reach Windows EFI consumer" >&2; exit 1;
  }
  if grep -aq WINPE_CONSUMER_RETURNED "$work/serial.log"; then
    echo "FAIL: direct Windows EFI Consumer returned to GRUB" >&2; exit 1;
  fi
  echo "UNVERIFIED: direct WinPE handoff reached EFI; inspect in-guest ISO mount" >&2
  exit 2
fi
grep -aq WINPE_TEST_BEGIN "$work/serial.log" || {
  echo "FAIL: GRUB test script never started" >&2; exit 1;
}
for marker in WINPE_EXTENT_OK WINPE_LOOPBACK_OK WINPE_CHAIN_READY WINPE_WIM_PATCH_COUNT=1 WINPE_ISO_FS=udf; do
  grep -aq "$marker" "$work/serial.log" || {
    echo "FAIL: $marker absent; see $work/serial.log" >&2; exit 1;
  }
done
# This historical fixture has a pinned Windows WIM jump binary. The Chain
# must match the proven-working descriptor size, not merely report success.
if test "${VENTOY_EXPECT_CHAIN_SIZE:-1667736}" != skip &&
   ! grep -aq "WINPE_CHAIN_SIZE=${VENTOY_EXPECT_CHAIN_SIZE:-1667736}" "$work/serial.log"; then
  echo "FAIL: WinPE Chain size differs from the historical working fixture" >&2
  exit 1
fi
if ! grep -aq '/WEPE/WEPE64.WIM \[SUCCESS\]' "$work/serial.log"; then
  echo "FAIL: WinPE WIM patch was found but not prepared" >&2; exit 1
fi
if grep -aq WINPE_ERROR_ "$work/serial.log"; then
  echo "FAIL: GRUB reported an error; see $work/serial.log" >&2; exit 1
fi
printf 'EFI consumer milestones:\n'
grep -aE 'WINPE_|StartImage|LoadImage|chain:|Error:|error:' "$work/serial.log" | tail -n 60 | cut -c1-240 || true
if test "$mode" = "--prepare"; then
  echo "PASS: WinPE UDF WIM Chain matches historical fixture; Windows guest mount not asserted by --prepare"
  pass=1
elif test "$mode" = "--invalid-params"; then
  if ! grep -aq WINPE_CONSUMER_RETURNED "$work/serial.log" ||
     ! grep -aEq 'StartImage status=0x0' "$work/serial.log"; then
    echo "NEGATIVE CONTROL CHANGED: forced isoefi=on case no longer produced the same failure" >&2
    exit 1
  fi
  echo "PASS (negative control): intentionally wrong isoefi=on/FirstTry arguments cause EFI failure"
  pass=1
else
  if grep -aq WINPE_CONSUMER_RETURNED "$work/serial.log"; then
    echo "FAIL: EFI Consumer returned to GRUB, inspect StartImage and EFI protocol trace" >&2
    exit 1
  fi
  if test "$mode" = "--trace"; then
    if grep -aq '\[VTOY-DP\] StartImage pre' "$work/serial.log" &&
       grep -aq 'CDROM(0x1' "$work/serial.log" &&
       ! grep -aq 'StartImage return status=Invalid Parameter' "$work/serial.log"; then
      for required in 'chain->override_chunk_num=4' \
                      'chain->virt_img_size_in_bytes=460988416' \
                      'os_param->vtoy_reserved=<0 0 1 1 0 0 0>'; do
        if ! grep -aFq "$required" "$work/serial.log"; then
          echo "FAIL: Consumer Chain differs from working reference ($required)" >&2
          exit 1
        fi
      done
      echo "PASS: UDF Chain fields match historical reference and EFI CDROM handoff is valid"
      pass=1
    else
      echo "FAIL: corrected EFI handoff did not match historical CDROM path" >&2
      exit 1
    fi
  else
    echo "UNVERIFIED: Windows user-space boot requires screen capture (QEMU=$status)" >&2
    exit 2
  fi
fi
