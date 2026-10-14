#!/bin/sh
# EFI/QEMU behavioral regression for a1ive G2 basic command compatibility.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
efi_build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}

for tool in qemu-system-x86_64 timeout mkfs.vfat mmd mcopy truncate gzip; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing tool: $tool" >&2; exit 1; }
done
for file in "$efi_build/grub-mkstandalone" "$ovmf_code" "$ovmf_vars"; do
  test -f "$file" || { echo "Missing file: $file" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-basic.XXXXXX")
passed=0
cleanup()
{
  if test "$passed" = 1 && test "${A1C_KEEP:-0}" != 1; then
    rm -rf "$work"
  else
    echo "Test output kept: $work" >&2
  fi
}
trap cleanup EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }

printf 'HELLO_A1C' > "$work/hello.txt"
cp "$work/hello.txt" "$work/same.txt"
printf 'BAD__A1C_' > "$work/different.txt"
gzip -n -c "$work/hello.txt" > "$work/hello.gz"

cat > "$work/grub.cfg" <<'CFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
set root=hd0
echo A1C_BASIC_BEGIN
if insmod cat; then echo A1C_OK_INSMOD_CAT; else echo A1C_ERROR_INSMOD_CAT; fi
if insmod blocklist; then echo A1C_OK_INSMOD_BLOCKLIST; else echo A1C_ERROR_INSMOD_BLOCKLIST; fi
if insmod cmp; then echo A1C_OK_INSMOD_CMP; else echo A1C_ERROR_INSMOD_CMP; fi
if insmod hexdump; then echo A1C_OK_INSMOD_HEXDUMP; else echo A1C_ERROR_INSMOD_HEXDUMP; fi
if insmod date; then echo A1C_OK_INSMOD_DATE; else echo A1C_ERROR_INSMOD_DATE; fi
insmod gzio

cat --set=contents (hd0)/hello.txt
if [ "$contents" = "HELLO_A1C" ]; then echo A1C_OK_CAT_SET_LONG; else echo "A1C_ERROR_CAT_SET_LONG=$contents"; fi
cat -s again (hd0)/hello.txt
if [ "$again" = "$contents" ]; then echo A1C_OK_CAT_SET_SHORT; else echo A1C_ERROR_CAT_SET_SHORT; fi

blocklist --set=blocks (hd0)/hello.txt
if [ -n "$blocks" ]; then echo A1C_OK_BLOCKLIST_SET; else echo A1C_ERROR_BLOCKLIST_SET; fi
blocklist -s partblocks -o 1 -l 4 (hd0)/hello.txt
if [ -n "$partblocks" ]; then echo A1C_OK_BLOCKLIST_RANGE; else echo A1C_ERROR_BLOCKLIST_RANGE; fi

if cmp -q (hd0)/hello.txt (hd0)/same.txt; then echo A1C_OK_CMP_QUIET_EQUAL; else echo A1C_ERROR_CMP_EQUAL; fi
if cmp --quiet (hd0)/hello.txt (hd0)/different.txt; then echo A1C_ERROR_CMP_MISMATCH; else echo A1C_OK_CMP_QUIET_DIFFER; fi

hexdump --quiet --length=5 (hd0)/hello.txt hexvar
if [ -n "$hexvar" ]; then echo "A1C_HEXDUMP_CAPTURE=$hexvar"; echo A1C_OK_HEXDUMP_SET; else echo A1C_ERROR_HEXDUMP_SET; fi
hexdump --decompress --quiet --length=5 (hd0)/hello.gz unpacked
if [ "$unpacked" = "$hexvar" ]; then echo A1C_OK_HEXDUMP_DECOMPRESS; else echo "A1C_ERROR_HEXDUMP_DECOMPRESS=$unpacked"; fi

date --set compactdate
if [ -n "$compactdate" ]; then echo A1C_OK_DATE_SET; else echo A1C_ERROR_DATE_SET; fi
date -m -s humandate
if [ -n "$humandate" ]; then echo A1C_OK_DATE_HUMAN_SET; else echo A1C_ERROR_DATE_HUMAN_SET; fi
if [ "$compactdate" != "$humandate" ]; then echo A1C_OK_DATE_DISTINCT; else echo A1C_ERROR_DATE_DISTINCT; fi

if checktime '*' '*' '*' '*' '*'; then echo A1C_OK_CHECKTIME_WILDCARDS; else echo A1C_ERROR_CHECKTIME_WILDCARDS; fi
if checktime 70 '*' '*' '*' '*'; then echo A1C_ERROR_CHECKTIME_INVALID; else echo A1C_OK_CHECKTIME_INVALID; fi

echo A1C_BASIC_DONE
halt
CFG

truncate -s 48M "$work/esp.img"
mkfs.vfat -F 16 -n A1CBASIC "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
for item in hello.txt same.txt different.txt hello.gz; do
  mcopy -i "$work/esp.img" "$work/$item" "::$item"
done

mods='serial terminal normal echo test halt fat part_msdos cat blocklist cmp hexdump date gzio'
preloaded='serial terminal normal echo test halt fat part_msdos'
if ! "$efi_build/grub-mkstandalone" -O x86_64-efi -d "$efi_build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules="$mods" --modules="$preloaded" \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1; then
  tail -n 16 "$work/pack.log" >&2
  fail "grub-mkstandalone failed"
fi
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$ovmf_vars" "$work/vars.fd"
if ! timeout 95s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  > "$work/qemu.log" 2>&1; then
  fail "QEMU did not exit cleanly"
fi

log="$work/serial.log"
for marker in A1C_BASIC_BEGIN A1C_BASIC_DONE \
  A1C_OK_INSMOD_CAT A1C_OK_INSMOD_BLOCKLIST A1C_OK_INSMOD_CMP \
  A1C_OK_INSMOD_HEXDUMP A1C_OK_INSMOD_DATE \
  A1C_OK_CAT_SET_LONG A1C_OK_CAT_SET_SHORT \
  A1C_OK_BLOCKLIST_SET A1C_OK_BLOCKLIST_RANGE \
  A1C_OK_CMP_QUIET_EQUAL A1C_OK_CMP_QUIET_DIFFER \
  A1C_OK_HEXDUMP_SET A1C_OK_HEXDUMP_DECOMPRESS \
  A1C_OK_DATE_SET A1C_OK_DATE_HUMAN_SET A1C_OK_DATE_DISTINCT \
  A1C_OK_CHECKTIME_WILDCARDS A1C_OK_CHECKTIME_INVALID; do
  grep -aq "$marker" "$log" || fail "missing GRUB marker $marker"
done
if grep -aq A1C_ERROR_ "$log"; then
  grep -a A1C_ERROR_ "$log" >&2
  fail "GRUB reported a failed behavior assertion"
fi
passed=1
echo 'PASS: G2 cat/blocklist/cmp/hexdump/date/checktime old command options'
