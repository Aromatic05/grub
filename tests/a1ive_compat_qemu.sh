#!/bin/sh
# QEMU regression for the a1ive commands GrubFM calls directly:
#   power.sh   -> reset -w / -c / -f / -s
#   init.sh    -> chainloader -b -t FILE; boot
#   proxmox.sh -> mkinitrd -c / -r / -a / -d; (md_initrd)
#   view.lua   -> Lua video library
#   memrw      -> Lua memrw library
# Only temporary images under a mktemp directory are written.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
efi_build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
pc_build=${A1C_PC_BUILD:-"$repo/build-i386-pc"}
grubfm=${A1C_GRUBFM_DIR:-"$repo/../../grub2-filemanager/boot/grubfm"}
font=${A1C_FONT:-/usr/share/grub/unicode.pf2}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}

for tool in qemu-system-x86_64 mkfs.vfat mmd mcopy cpio gzip socat python3 timeout xorriso; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing: $tool" >&2; exit 1; }
done
for path in "$efi_build/grub-mkstandalone" "$pc_build/grub-mkstandalone" \
            "$grubfm/view.lua" "$font" "$ovmf_code" "$ovmf_vars"; do
  test -f "$path" || { echo "Missing file: $path" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-compat.XXXXXX")
monitor_dir=$(mktemp -d /tmp/a1ive-monitor.XXXXXX)
pass=0
cleanup()
{
  rm -f "$monitor_dir"/*.sock 2>/dev/null || :
  rmdir "$monitor_dir" 2>/dev/null || :
  if test "$pass" = 1 && test "${A1C_KEEP:-0}" != 1; then rm -rf "$work"
  else echo "Test output kept: $work" >&2; fi
}
trap cleanup EXIT HUP INT TERM

fail()
{
  echo "FAIL: $*" >&2
  exit 1
}

# wait_for FILE PATTERN SECONDS
wait_for()
{
  n=0
  while ! grep -aq "$2" "$1" 2>/dev/null; do
    n=$((n + 1))
    test "$n" -le "$(($3 * 4))" || return 1
    sleep 0.25
  done
}

monitor()
{
  printf '%s\n' "$2" | socat -T 2 - "UNIX-CONNECT:$1" | tr -d '\r'
}

efi_modules='serial terminal normal echo test search search_fs_file fat part_msdos
  reboot halt chain boot newc gzio cat ls lua memrw regexp videoinfo getkey
  efi_gop gfxterm font png sleep'

# ---------------------------------------------------------------- fixtures
mkdir -p "$work/initrd"
printf 'ORIGINAL-INIT\n' > "$work/initrd/init"
printf 'payload\n' > "$work/initrd/keep"
(cd "$work/initrd" && printf 'init\nkeep\n' | cpio -o -H newc --quiet) |
  gzip -9 > "$work/initrd.gz"
printf 'REPLACED-INIT\n' > "$work/newinit"
printf '\104\063\042\021' > "$work/bytes.bin"
cat > "$work/memrw.lua" <<'LUA'
print ("A1C_MEMRW_READ=" .. string.format ("%x", memrw.read_dword (0x2000000)))
memrw.write_word (0x2000000, 0xbeef)
memrw.write_byte (0x2000003, 0x5a)
LUA
cat > "$work/video.lua" <<'LUA'
w, h = video.get_info ()
print ("A1C_VIDEO_INFO=" .. w .. "x" .. h)
b = video.bitmap_load (grub.getenv ("missing_png"))
print ("A1C_BITMAP_NIL=" .. tostring (b == nil))
b = video.bitmap_load (grub.getenv ("data_png"))
w, h = video.bitmap_info (b)
s = video.bitmap_rescale (b, 32, 24)
sw, sh = video.bitmap_info (s)
video.bitmap_close (s)
video.bitmap_close (b)
print ("A1C_BITMAP=" .. w .. "x" .. h .. "/" .. sw .. "x" .. sh)
LUA

python3 -I - "$work/red.png" <<'PY'
import struct, sys, zlib
w, h = 64, 48
raw = b"".join(b"\0" + b"\xff\x00\x00" * w for _ in range(h))
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
open(sys.argv[1], "wb").write(png)
PY

# Child EFI application used by chainloader -b: prints and returns.
cat > "$work/child.cfg" <<'CFG'
serial --speed=115200
terminal_output serial
echo A1C_CHILD_RAN
exit
CFG
"$efi_build/grub-mkstandalone" -O x86_64-efi -d "$efi_build/grub-core" \
  -o "$work/child.efi" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules='serial terminal normal echo minicmd' \
  --modules='serial terminal normal echo minicmd' \
  "boot/grub/grub.cfg=$work/child.cfg" > "$work/child-pack.log" 2>&1

# ------------------------------------------------------------- EFI image
mk_efi()
{
  # mk_efi NAME CFG
  "$efi_build/grub-mkstandalone" -O x86_64-efi -d "$efi_build/grub-core" \
    -o "$work/$1.efi" --fonts='' --locales='' --themes='' --compress=no \
    --install-modules="$efi_modules" --modules="$efi_modules" \
    "boot/grub/grub.cfg=$2" > "$work/$1-pack.log" 2>&1
  truncate -s 48M "$work/$1.img"
  mkfs.vfat -F 16 -n A1CTEST "$work/$1.img" >/dev/null
  mmd -i "$work/$1.img" ::/EFI ::/EFI/BOOT
  mcopy -i "$work/$1.img" "$work/$1.efi" ::/EFI/BOOT/BOOTX64.EFI
  printf 'x' > "$work/a1c.marker"
  for f in a1c.marker child.efi initrd.gz newinit bytes.bin red.png memrw.lua video.lua; do
    mcopy -i "$work/$1.img" "$work/$f" "::/$f"
  done
  mcopy -i "$work/$1.img" "$grubfm/view.lua" ::/view.lua
  mcopy -i "$work/$1.img" "$font" ::/unicode.pf2
  cp "$ovmf_vars" "$work/$1.vars"
}

# run_efi NAME -> starts QEMU in background, pid in $qemu_pid
run_efi()
{
  timeout 120s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 -m 512M \
    -display none -vga std -no-user-config -nodefaults -device VGA \
    -action reboot=shutdown,shutdown=pause \
    -trace qemu_system_shutdown_request \
    -monitor "unix:$monitor_dir/$1.sock,server,nowait" \
    -serial "file:$work/$1.serial" \
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
    -drive "if=pflash,format=raw,file=$work/$1.vars" \
    -drive "if=virtio,format=raw,file=$work/$1.img" \
    > "$work/$1.qemu" 2>&1 &
  qemu_pid=$!
}

stop_qemu()
{
  monitor "$monitor_dir/$1.sock" quit >/dev/null 2>&1 || true
  wait "$qemu_pid" || true
}

# Wait until the guest requested a reset/shutdown and QEMU paused.
wait_paused()
{
  wait_for "$work/$1.qemu" qemu_system_shutdown_request 60 ||
    fail "$1: no reset/shutdown request"
}

# ------------------------------------------------- EFI functional script
cat > "$work/main.cfg" <<'CFG'
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
search --file --set=data /a1c.marker
echo A1C_BEGIN

# mkinitrd: same sequence as grubfm distro/proxmox.sh.
mkinitrd -c md_initrd ($data)/initrd.gz
mkinitrd -r md_initrd init FUCK
mkinitrd -a md_initrd ($data)/newinit init
echo A1C_INIT_BEGIN
cat (md_initrd)/init
echo A1C_FUCK_BEGIN
cat (md_initrd)/FUCK
echo A1C_KEEP_BEGIN
cat (md_initrd)/keep
echo A1C_INITRD_END
if mkinitrd -r md_initrd keep toolong; then echo A1C_ERROR_REN_LEN; else echo A1C_REN_LEN_REJECTED; fi
if mkinitrd -r md_initrd missing absentx; then echo A1C_ERROR_REN_MISSING; else echo A1C_REN_MISSING_REJECTED; fi
mkinitrd -d md_initrd
if cat (md_initrd)/init; then echo A1C_ERROR_DELETE; else echo A1C_DELETE_OK; fi

# Lua memrw against a fixed buffer holding 44 33 22 11.
loadfile -a 0x2000000 ($data)/bytes.bin
lua ($data)/memrw.lua
read_dword -v value 0x2000000
echo "A1C_MEMRW_WRITE=$value"

# Lua video: grubfm view.lua on a 64x48 red PNG.
loadfont ($data)/unicode.pf2
set gfxmode=640x480
terminal_output --append gfxterm
set missing_png=($data)/absent.png
set data_png=($data)/red.png
lua ($data)/video.lua
set bmp_path=($data)/red.png
terminal_input --append console
echo A1C_VIEW_START
lua ($data)/view.lua
terminal_output serial
terminal_input serial
echo A1C_VIEW_DONE

# chainloader -b: grubfm init.sh runs "chainloader -b -t FILE; boot".
chainloader -b -t ($data)/child.efi
echo A1C_CHAIN_RETURNED
if boot; then echo A1C_ERROR_LOADER_LEFT; else echo A1C_LOADER_CLEARED; fi
echo A1C_DONE
reset -s
CFG

mk_efi main "$work/main.cfg"
run_efi main
wait_for "$work/main.serial" A1C_VIEW_START 90 || fail "main: view.lua not reached"
sleep 5
monitor "$monitor_dir/main.sock" "screendump $work/view.ppm" >/dev/null
# view.lua blocks in input.getkey until a key arrives.
monitor "$monitor_dir/main.sock" "sendkey ret" >/dev/null
wait_paused main
stop_qemu main
log="$work/main.serial"

grep -aq A1C_DONE "$log" || fail "main script did not finish"
if grep -aq -e A1C_ERROR_ -e 'Lua:' "$log"; then
  grep -a A1C_ERROR_ "$log" >&2
  fail "unexpected acceptance"
fi
section()
{
  sed -n "/$1/,/$2/p" "$log" | tr -d '\r' | grep -av "^$1\$" | grep -av "^$2\$"
}
test "$(section A1C_INIT_BEGIN A1C_FUCK_BEGIN)" = REPLACED-INIT ||
  fail "mkinitrd -a did not provide the new init"
test "$(section A1C_FUCK_BEGIN A1C_KEEP_BEGIN)" = ORIGINAL-INIT ||
  fail "mkinitrd -r did not keep the original init as FUCK"
test "$(section A1C_KEEP_BEGIN A1C_INITRD_END)" = payload ||
  fail "mkinitrd lost an unrelated archive member"
for m in A1C_REN_LEN_REJECTED A1C_REN_MISSING_REJECTED A1C_DELETE_OK \
         A1C_MEMRW_READ=11223344 A1C_MEMRW_WRITE=5a22beef \
         A1C_VIDEO_INFO=640x480 A1C_BITMAP_NIL=true A1C_BITMAP=64x48/32x24 \
         A1C_VIEW_DONE A1C_CHILD_RAN A1C_CHAIN_RETURNED A1C_LOADER_CLEARED; do
  grep -aq "$m" "$log" || fail "missing $m"
done
# The child must run inside "chainloader -b", before the command returns.
child_line=$(grep -an A1C_CHILD_RAN "$log" | head -n 1 | cut -d: -f1)
ret_line=$(grep -an A1C_CHAIN_RETURNED "$log" | head -n 1 | cut -d: -f1)
test "$child_line" -lt "$ret_line" || fail "chainloader -b did not start the image"

# view.lua: black screen, the 64x48 red PNG centred, text at the bottom.
python3 -I - "$work/view.ppm" <<'PY' || fail "view.lua frame mismatch"
import sys
data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
w, h = map(int, parts[1].split())
pix = parts[3]
def at(x, y):
    o = (y * w + x) * 3
    return tuple(pix[o:o + 3])
assert (w, h) == (640, 480), (w, h)
assert at(320, 240) == (255, 0, 0), at(320, 240)
assert at(320 - 31, 240 - 23) == (255, 0, 0), at(289, 217)
assert at(320 - 40, 240) == (0, 0, 0), at(280, 240)
assert at(639, 0) == (0, 0, 0), at(639, 0)
assert any(at(x, y) == (255, 255, 255) for x in range(0, 200) for y in range(464, 480))
PY
grep -a 'qemu_system_shutdown_request' "$work/main.qemu" | grep -aq 'reason=6' ||
  fail "reset -s did not request a guest shutdown"
echo "PASS: EFI mkinitrd, Lua memrw, Lua video/view.lua, chainloader -b, reset -s"

# ------------------------------------------------------------ EFI resets
reset_efi()
{
  # reset_efi NAME ARG: QMP guest:true reset must precede host cleanup.
  printf 'serial --speed=115200\nterminal_output serial\necho A1C_RESET\nreset %s\necho A1C_ERROR_RETURNED\n' \
    "$2" > "$work/$1.cfg"
  mk_efi "$1" "$work/$1.cfg"
  python3 -I "$repo/tests/a1ive_qmp_guest_event.py" \
    --expect guest-reset --timeout 40 \
    --qmp-log "$work/$1.qmp.json" --qemu-log "$work/$1.qemu" -- \
    qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 -m 512M \
    -display none -vga std -no-user-config -nodefaults -device VGA \
    -action reboot=shutdown,shutdown=pause \
    -serial "file:$work/$1.serial" \
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
    -drive "if=pflash,format=raw,file=$work/$1.vars" \
    -drive "if=virtio,format=raw,file=$work/$1.img" \
    || fail "$1: QMP guest reset was not observed"
  grep -aq A1C_RESET "$work/$1.serial" || fail "$1: script not reached"
  ! grep -aq A1C_ERROR_RETURNED "$work/$1.serial" || fail "$1: reset returned"
}

has_os_indications()
{
  python3 -I - "$1" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
name = "OsIndications".encode("utf-16-le") + b"\0\0"
sys.exit(0 if name in data else 1)
PY
}

if test "${A1C_SKIP_EFI_RESET:-0}" = 1; then
  echo "SKIP: EFI reset -c/-w/-f (A1C_SKIP_EFI_RESET=1 explicitly requested)"
else
  reset_efi efi_cold -c
  reset_efi efi_warm -w
  ! has_os_indications "$work/efi_cold.vars" || fail "reset -c touched OsIndications"
  reset_efi efi_fwui -f
  has_os_indications "$work/efi_fwui.vars" || fail "reset -f did not set OsIndications"
  echo "PASS: EFI reset -c / -w / -f"
fi

# ------------------------------------------------------------ BIOS resets
reset_pc()
{
  # reset_pc NAME ARGS -> prints the BDA reset flag at 0x472
  printf 'serial --speed=115200\nterminal_output serial\necho A1C_RESET\nreset %s\necho A1C_ERROR_RETURNED\n' \
    "$2" > "$work/$1.cfg"
  if ! "$pc_build/grub-mkstandalone" -O i386-pc-eltorito -d "$pc_build/grub-core" \
    -o "$work/$1.eltorito.img" --fonts='' --locales='' --themes='' \
    --install-modules='serial terminal normal echo reboot halt biosdisk iso9660' \
    --modules='serial terminal normal echo reboot halt biosdisk iso9660' \
    "boot/grub/grub.cfg=$work/$1.cfg" > "$work/$1-pack.log" 2>&1; then
    tail -n 5 "$work/$1-pack.log" >&2
    fail "$1: could not construct BIOS boot image"
  fi
  # grub-mkstandalone emits an El Torito boot image, NOT an ISO9660 volume.
  # SeaBIOS -cdrom requires an actual ISO containing its El Torito catalog.
  mkdir -p "$work/$1-iso-root/boot/grub"
  cp "$work/$1.eltorito.img" "$work/$1-iso-root/boot/grub/eltorito.img"
  if ! xorriso -as mkisofs -quiet -R \
    -b boot/grub/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table \
    -o "$work/$1.iso" "$work/$1-iso-root" > "$work/$1-iso.log" 2>&1; then
    tail -n 5 "$work/$1-iso.log" >&2
    fail "$1: could not package El Torito boot image in ISO9660"
  fi
  python3 -I "$repo/tests/a1ive_qmp_guest_event.py" \
    --expect guest-reset --timeout 40 \
    --qmp-log "$work/$1.qmp.json" --qemu-log "$work/$1.qemu" \
    --bda-output "$work/$1.bda" -- \
    qemu-system-x86_64 -machine pc,accel=tcg -m 256M -display none \
    -action reboot=shutdown,shutdown=pause \
    -serial "file:$work/$1.serial" -cdrom "$work/$1.iso" -boot d \
    || fail "$1: QMP guest reset was not observed"
  grep -aq A1C_RESET "$work/$1.serial" || fail "$1: script not reached"
  ! grep -aq A1C_ERROR_RETURNED "$work/$1.serial" || fail "$1: reset returned"
  grep -aoE '0*472: 0x[0-9a-fA-F]+' "$work/$1.bda" |
    sed 's/.*: //' > "$work/$1.flag"
}

reset_pc pc_cold -c
reset_pc pc_warm -w
test "$(cat "$work/pc_cold.flag")" = 0x0000 || fail "BIOS reset -c flag: $(cat "$work/pc_cold.flag")"
test "$(cat "$work/pc_warm.flag")" = 0x1234 || fail "BIOS reset -w flag: $(cat "$work/pc_warm.flag")"
echo "PASS: BIOS reset -c / -w (BDA 0x472 flag)"

pass=1
