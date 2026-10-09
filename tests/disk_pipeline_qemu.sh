#!/bin/sh
# End-to-end GRUB disk pipeline regression on isolated QEMU file images.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${1:-"$repo/build-x86_64-efi"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}

for tool in mkfs.vfat mmd mcopy qemu-system-x86_64 qemu-img xz gzip timeout sha256sum; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "Missing test dependency: $tool" >&2
    exit 1
  }
done
test -f "$ovmf_code" && test -f "$ovmf_vars" || {
  echo "Set OVMF_CODE and OVMF_VARS to the OVMF firmware files." >&2
  exit 1
}
test -x "$build/grub-mkstandalone" && test -f "$build/grub-core/loopback.mod" || {
  echo "Build GRUB for x86_64-efi first." >&2
  exit 1
}

test_work=$(mktemp -d "${TMPDIR:-/tmp}/grub-disk-pipeline.XXXXXX")
pass=0
cleanup()
{
  if [ "$pass" = 1 ]; then
    rm -rf "$test_work"
  else
    echo "Test data and serial log kept at: $test_work" >&2
  fi
}
trap cleanup EXIT HUP INT TERM

truncate -s 32M "$test_work/raw.img"
mkfs.vfat -F 16 -n INNER "$test_work/raw.img" >/dev/null
printf 'PIPELINE_TEST_MARKER_2026\n' > "$test_work/MARKER.TXT"
mcopy -i "$test_work/raw.img" "$test_work/MARKER.TXT" ::/MARKER.TXT

xz -1 -c "$test_work/raw.img" > "$test_work/raw.img.xz"
gzip -1 -c "$test_work/raw.img.xz" > "$test_work/raw.img.xz.gz"
gzip -1 -c "$test_work/raw.img" > "$test_work/raw.img.gz"
xz -1 -c "$test_work/raw.img.gz" > "$test_work/raw.img.gz.xz"
qemu-img convert -f raw -O vpc -o subformat=dynamic \
  "$test_work/raw.img" "$test_work/raw.vhd"
xz -1 -c "$test_work/raw.vhd" > "$test_work/raw.vhd.xz"
# Retain the VHD footer so FORMAT probe succeeds, but invalidate the
# dynamic-header magic. This exercises failure *inside* the parser.
cp "$test_work/raw.vhd" "$test_work/bad.vhd"
printf 'X' | dd of="$test_work/bad.vhd" bs=1 seek=512 count=1 \
  conv=notrunc status=none
sha256sum "$test_work/raw.img.xz" > "$test_work/source.sha256"

truncate -s 64M "$test_work/data.img"
mkfs.vfat -F 16 -n DATATEST "$test_work/data.img" >/dev/null
for file in raw.img raw.img.xz raw.img.xz.gz raw.img.gz.xz raw.img.gz raw.vhd.xz bad.vhd; do
  mcopy -i "$test_work/data.img" "$test_work/$file" ::/"$file"
done

cat > "$test_work/grub.cfg" <<'GRUBCFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo PIPELINE_TEST_BEGIN

loopback raw (hd1)/raw.img
cat (raw)/MARKER.TXT
loopback -d raw

loopback zx (hd1)/raw.img.xz
cat (zx)/MARKER.TXT
dd -x 50495045 -o (zx)0+1 -b 4 -c 1
echo PIPELINE_COW_WRITTEN
hexdump -s 0 -n 16 (zx)0+1
loopback -d zx

loopback zx (hd1)/raw.img.xz
echo PIPELINE_COW_REOPENED
hexdump -s 0 -n 16 (zx)0+1
cat (zx)/MARKER.TXT
loopback -d zx

loopback nested (hd1)/raw.img.xz.gz
cat (nested)/MARKER.TXT
loopback -d nested

loopback nested (hd1)/raw.img.gz.xz
cat (nested)/MARKER.TXT
loopback -d nested

vhd vdx (hd1)/raw.vhd.xz
cat (vdx)/MARKER.TXT
loopback -d vdx

if vhd broken (hd1)/bad.vhd; then
  echo PIPELINE_ERROR_MALFORMED_VHD_ACCEPTED
else
  echo PIPELINE_MALFORMED_VHD_REJECTED
fi
# The parser's failed allocation/cleanup must not poison the next open.
vhd recovered (hd1)/raw.vhd.xz
cat (recovered)/MARKER.TXT
loopback -d recovered

loopback -m ram (hd1)/raw.img
cat (ram)/MARKER.TXT
loopback -d ram

loopback -m ram (hd1)/raw.img.xz
cat (ram)/MARKER.TXT
loopback -d ram

loopback gz (hd1)/raw.img.gz
cat (gz)/MARKER.TXT
loopback -d gz

loopback -D compat (hd1)/raw.img.gz
cat (compat)/MARKER.TXT
loopback -d compat

loopback -l bl (hd1)/raw.img
cat (bl)/MARKER.TXT
loopback -d bl

echo PIPELINE_EFI_XZ_BEGIN
map -n (hd1)/raw.img.xz evxz
cat (evxz)/MARKER.TXT
map -x evxz

echo PIPELINE_EFI_FORMAT_BEGIN
map -n -F (hd1)/raw.vhd.xz evfmt
cat (evfmt)/MARKER.TXT
map -x evfmt

echo PIPELINE_EFI_COW_BEGIN
map -n -C (hd1)/raw.img evcow
cat (evcow)/MARKER.TXT
dd -x 54455354 -o (evcow)0+1 -b 4 -c 1
hexdump -s 0 -n 16 (evcow)0+1
map -x evcow
echo PIPELINE_EFI_COW_REOPENED
map -n -C (hd1)/raw.img evcow
hexdump -s 0 -n 16 (evcow)0+1
map -x evcow

loopback -C rw (hd1)/raw.img
dd -x 41424344 -o (rw)0+1 -b 4 -c 1
echo PIPELINE_RAW_COW_WRITTEN
hexdump -s 0 -n 16 (rw)0+1
loopback -d rw

if loopback mem (hd1)/raw.img; then
  echo PIPELINE_ERROR_RESERVED_NOT_REJECTED
else
  echo PIPELINE_RESERVED_REJECTED
fi

if loopback hd1 (hd1)/raw.img; then
  echo PIPELINE_ERROR_PHYSICAL_NOT_REJECTED
else
  echo PIPELINE_PHYSICAL_REJECTED
fi

if vhd invalid (hd1)/raw.img; then
  echo PIPELINE_ERROR_FORMAT_NOT_REJECTED
else
  echo PIPELINE_FORMAT_REJECTED
fi

loopback duplicate (hd1)/raw.img
if loopback duplicate (hd1)/raw.img; then
  echo PIPELINE_ERROR_DUPLICATE_NOT_REJECTED
else
  echo PIPELINE_DUPLICATE_REJECTED
fi
loopback -d duplicate

echo PIPELINE_TEST_DONE
halt
GRUBCFG

"$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
  -o "$test_work/BOOTX64.EFI" --fonts='' --locales='' --compress=no \
  --modules='serial terminal normal echo ls cat halt part_msdos fat loopback xzio gzio vhd map dd hexdump' \
  "boot/grub/grub.cfg=$test_work/grub.cfg" > "$test_work/pack.log" 2>&1

truncate -s 32M "$test_work/esp.img"
mkfs.vfat -F 16 -n PIPEESP "$test_work/esp.img" >/dev/null
mmd -i "$test_work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$test_work/esp.img" "$test_work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$ovmf_vars" "$test_work/OVMF_VARS.fd"

qemu_status=0
timeout 45s qemu-system-x86_64 -machine q35,accel=tcg -m 512M \
  -display none -monitor none -no-reboot \
  -serial "file:$test_work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$test_work/OVMF_VARS.fd" \
  -drive "if=virtio,format=raw,readonly=on,file=$test_work/esp.img" \
  -drive "if=virtio,format=raw,readonly=on,file=$test_work/data.img" \
  || qemu_status=$?
if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
  echo "QEMU failed with status $qemu_status" >&2
  exit 1
fi

grep -q 'PIPELINE_TEST_DONE' "$test_work/serial.log"
marker_count=$(grep -c 'PIPELINE_TEST_MARKER_2026' "$test_work/serial.log")
test "$marker_count" -eq 15 || {
  echo "Expected 15 successful file reads, got $marker_count" >&2
  exit 1
}
grep -q '50 49 50 45' "$test_work/serial.log"
grep -q '41 42 43 44' "$test_work/serial.log"
grep -q '54 45 53 54' "$test_work/serial.log"
grep -q 'PIPELINE_EFI_COW_REOPENED' "$test_work/serial.log"
grep -q 'PIPELINE_EFI_FORMAT_BEGIN' "$test_work/serial.log"
grep -q 'eb 3c 90 6d' "$test_work/serial.log"
for marker in RESERVED_REJECTED PHYSICAL_REJECTED FORMAT_REJECTED DUPLICATE_REJECTED MALFORMED_VHD_REJECTED; do
  grep -q "PIPELINE_$marker" "$test_work/serial.log" || {
    echo "Failed check: $marker" >&2
    exit 1
  }
done
if grep -q 'PIPELINE_ERROR_' "$test_work/serial.log"; then
  echo "Unexpected acceptance of a failing operation" >&2
  exit 1
fi
(cd "$test_work" && sha256sum -c source.sha256) >/dev/null
grep '^loopback:' "$build/grub-core/moddep.lst" | grep -qv 'vhd'

pass=1
echo "PASS: RAW, nested XZ/Gzip, VHD.XZ, parser rollback, RAM, blocklist, COW and name conflicts"
