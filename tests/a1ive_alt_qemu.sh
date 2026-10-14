#!/bin/sh
# A1ive-compatible EFI chainloader regression:
# real PE execution, DIR64 relocation, and malformed-header rejection.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
efi_build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}

for tool in clang lld-link python3 objdump qemu-system-x86_64 \
            timeout mkfs.vfat mmd mcopy; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "Missing tool: $tool" >&2
    exit 1
  }
done

for file in "$efi_build/grub-mkstandalone" "$ovmf_code" "$ovmf_vars"; do
  test -f "$file" || {
    echo "Missing file: $file" >&2
    exit 1
  }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-alt.XXXXXX")
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

cat > "$work/child.c" <<'C'
typedef unsigned long EFI_STATUS;

#ifdef RELOC
static const char text[] = "A1C_ALT_RELOC_EXECUTED\r\n";
static const char *volatile message = text;
#else
#define message "A1C_ALT_EXECUTED\r\n"
#endif

static void serial_write(const char *s)
{
  while (*s)
    {
      unsigned char c = (unsigned char) *s++;
      __asm__ __volatile__ ("outb %0, %1" :
                            : "a" (c), "Nd" ((unsigned short) 0x3f8));
    }
}

__attribute__((ms_abi)) EFI_STATUS efi_main(void *image, void *system_table)
{
  (void) image;
  (void) system_table;
  serial_write(message);
  return 0;
}
C

build_child()
{
  name=$1
  shift
  clang --target=x86_64-pc-windows-msvc -ffreestanding \
    -fno-stack-protector -fno-builtin -c "$@" \
    "$work/child.c" -o "$work/$name.obj"
  lld-link /subsystem:efi_application /entry:efi_main /machine:x64 \
    /nodefaultlib "/out:$work/$name.efi" "$work/$name.obj"
}

build_child normal
build_child reloc -DRELOC=1
objdump -h "$work/reloc.efi" | grep -q '[[:space:]]\.reloc[[:space:]]' || {
  echo "Relocation test fixture has no .reloc section" >&2
  exit 1
}

# Make SizeOfHeaders larger than the source file to verify OOB rejection.
python3 -I - "$work/normal.efi" "$work/bad.efi" <<'PY'
import pathlib, sys
data = bytearray(pathlib.Path(sys.argv[1]).read_bytes())
pe = int.from_bytes(data[0x3c:0x40], "little")
field = pe + 4 + 20 + 60  # PE32+ optional header: SizeOfHeaders
assert data[pe:pe + 4] == b"PE\x00\x00"
data[field:field + 4] = (0x7fffffff).to_bytes(4, "little")
pathlib.Path(sys.argv[2]).write_bytes(data)
PY

cat > "$work/grub.cfg" <<'CFG'
serial --speed=115200
terminal_output serial
set root=hd0
insmod chain
echo A1C_ALT_BEGIN
chainloader --alt --boot (hd0)/normal.efi
chainloader -a -b (hd0)/reloc.efi
if chainloader --alt --boot (hd0)/bad.efi; then
  echo A1C_ALT_BAD_ACCEPTED
else
  echo A1C_ALT_BAD_REJECTED
fi
echo A1C_ALT_DONE
halt
CFG

"$efi_build/grub-mkstandalone" -O x86_64-efi -d "$efi_build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' \
  --install-modules='serial terminal normal echo chain boot fat part_msdos halt' \
  --modules='serial terminal normal echo chain boot fat part_msdos halt' \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1

truncate -s 48M "$work/esp.img"
mkfs.vfat -F 16 "$work/esp.img" >/dev/null
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
for name in normal reloc bad; do
  mcopy -i "$work/esp.img" "$work/$name.efi" "::/$name.efi"
done
cp "$ovmf_vars" "$work/vars.fd"

if ! timeout 70s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  > "$work/qemu.log" 2>&1; then
  echo "FAIL: chainloader --alt QEMU did not shutdown cleanly" >&2
  exit 1
fi

for marker in A1C_ALT_BEGIN A1C_ALT_EXECUTED A1C_ALT_RELOC_EXECUTED \
              A1C_ALT_BAD_REJECTED A1C_ALT_DONE; do
  grep -aq "$marker" "$work/serial.log" || {
    echo "FAIL: missing $marker" >&2
    exit 1
  }
done
if grep -aq A1C_ALT_BAD_ACCEPTED "$work/serial.log"; then
  echo "FAIL: malformed PE was accepted" >&2
  exit 1
fi

passed=1
echo "PASS: --alt/--boot/-a/-b PE execution, DIR64 relocation, malformed PE rejection"
