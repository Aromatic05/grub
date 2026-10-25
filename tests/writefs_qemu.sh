#!/bin/sh
# Real FAT/exFAT/NTFS write operations, persistence and failed-move regression.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
platform=${GRUB_PLATFORM:-x86_64-efi}
build=${GRUB_BUILD:-"$repo/build-$platform"}
code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
for tool in python3 qemu-system-x86_64 timeout mkfs.fat mkfs.exfat mkntfs ntfscp ntfscat ntfsfix fsck.fat fsck.exfat mcopy mmd truncate sfdisk dd ntfslabel; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/grub-writefs.XXXXXX")
passed=0
cleanup() {
  if test "$passed" = 1 && test "${WRITEFS_KEEP:-0}" != 1; then rm -rf "$work"; else echo "Evidence: $work" >&2; fi
}
trap cleanup EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }
python3 - "$work" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
p.joinpath('payload.bin').write_bytes(bytes(range(256)) * 1025)
p.joinpath('short.txt').write_bytes(b'hello')
p.joinpath('huge.bin').write_bytes(bytes(range(256)) * 32768)
p.joinpath('gap.bin').write_bytes(b'hello' + bytes(4091) + b'END')
p.joinpath('hiberfil.sys').write_bytes(b'hibr' + bytes(4092))
p.joinpath('invalid-upcase').write_bytes(b'X')
p.joinpath('bind.ini').write_text('[section]\nkey=hello\n')
p.joinpath('bind-runtime-reload.lua').write_text(
    'assert(gbk.toutf8("Hello") == "Hello")\n'
    'assert(type(ini.get) == "function")\n'
    'assert(type(input.getkey_noblock) == "function")\n'
    'assert(type(writefs.supported) == "function")\n')
p.joinpath('bind-input.lua').write_text(
    'local a, b = input.getkey_noblock()\n'
    'assert(type(a) == "number" and type(b) == "number")\n'
    'assert(a >= 0 and a <= 255 and b >= 0 and b <= 255)\n')
p.joinpath('bind-wim.lua').write_text(
    'assert(wim.file_exist("(missing)/not-a-wim", "/Windows") == false)\n'
    'assert(wim.is64("(missing)/not-a-wim") == false)\n'
    'assert(wim.image_count("(missing)/not-a-wim") == 0)\n'
    'assert(wim.boot_index("(missing)/not-a-wim") == 0)\n')
p.joinpath('bind-before.lua').write_text(
    'assert(gbk == nil)\n'
    'assert(ini == nil)\n'
    'assert(writefs ~= nil)\n')
p.joinpath('bind-conv.lua').write_text(
    'assert(gbk.fromutf8("Hello") == "Hello")\n'
    'assert(gbk.toutf8("Hello") == "Hello")\n'
    'assert(gbk.tosimp("Hello") == "Hello")\n'
    'assert(not pcall(gbk.toutf8, 42))\n'
    'bind_saved_gbk = gbk.toutf8\n')
p.joinpath('bind-stale-conv.lua').write_text(
    'assert(not pcall(bind_saved_gbk, "Hello"))\n'
    'assert(gbk.toutf8 == nil)\n')
p.joinpath('bind-reload-conv.lua').write_text(
    'assert(gbk.toutf8("Hello") == "Hello")\n'
    'assert(not pcall(bind_saved_gbk, "Hello"))\n')
p.joinpath('bind-ini-open.lua').write_text(
    'bind_ini_handle = ini.load("(" .. grub.getenv("esp") .. ")/bind.ini")\n'
    'assert(type(bind_ini_handle) == "userdata")\n'
    'assert(ini.get(bind_ini_handle, "section", "key") == "hello")\n'
    'bind_saved_ini = ini.get\n')
p.joinpath('bind-ini-pinned.lua').write_text(
    'assert(ini.get(bind_ini_handle, "section", "key") == "hello")\n')
p.joinpath('bind-ini-close.lua').write_text(
    'ini.free(bind_ini_handle)\n'
    'assert(not pcall(ini.free, bind_ini_handle))\n'
    'assert(not pcall(ini.get, bind_ini_handle, "section", "key"))\n'
    'bind_ini_handle = nil\n'
    'collectgarbage("collect")\n')
p.joinpath('bind-ini-stale.lua').write_text(
    'assert(not pcall(bind_saved_ini, nil, "section", "key"))\n'
    'assert(ini.get == nil)\n')
p.joinpath('bind-ini-reload.lua').write_text(
    'local h = ini.load("(" .. grub.getenv("esp") .. ")/bind.ini")\n'
    'assert(h ~= nil)\n'
    'assert(ini.get(h, "section", "key") == "hello")\n'
    'assert(not pcall(bind_saved_ini, h, "section", "key"))\n'
    'ini.free(h)\n')
p.joinpath('supported.lua').write_text(
    'assert(fat == nil)\n'
    'local expected = {fat12=true, fat16=true, fat32=true, exfat=true, ntfs=true}\n'
    'local supported = writefs.supported()\n'
    'local count = 0\n'
    'for _, fs in ipairs(supported) do\n'
    '  assert(expected[fs.name], fs.name)\n'
    '  assert(fs.driver == (fs.name == "ntfs" and "ntfs3g" or "fatfs"))\n'
    '  assert(type(fs.loaded) == "boolean")\n'
    '  count = count + 1\n'
    'end\n'
    'assert(count == 5)\n')
p.joinpath('read.lua').write_text(
    'wf_handle = writefs.open("1:/test/big.bin", 1)\n'
    'assert(writefs.read(wf_handle, 4) == string.char(0, 1, 2, 3))\n'
    'local n = writefs.open("2:/test/big.bin", 1)\n'
    'assert(writefs.read(n, 4) == string.char(0, 1, 2, 3))\n'
    'writefs.close(n)\n'
    'assert(not pcall(function() writefs.read(n, 1) end))\n')
p.joinpath('close.lua').write_text(
    'writefs.close(wf_handle)\n'
    'assert(not pcall(function() writefs.read(wf_handle, 1) end))\n'
    'assert(not pcall(function() writefs.close(wf_handle) end))\n'
    'wf_handle = nil\n')
p.joinpath('write.lua').write_text(
    'for _, num in ipairs({1, 2, 3}) do\n'
    '  local path = num .. ":/test/fromlua"\n'
    '  local h = writefs.open(path, 6)\n'
    '  assert(not pcall(function() writefs.read(h, 1) end))\n'
    '  assert(writefs.write(h, "abc") == 3)\n'
    '  writefs.seek(h, 1)\n'
    '  assert(writefs.write(h, "Z") == 1)\n'
    '  assert(writefs.size(h) == 3)\n'
    '  writefs.close(h)\n'
    '  local r = writefs.open(path, 1)\n'
    '  assert(writefs.read(r, 3) == "aZc")\n'
    '  assert(writefs.eof(r))\n'
    '  writefs.close(r)\n'
    '  local rw = writefs.open(path, 3)\n'
    '  assert(writefs.read(rw, 3) == "aZc")\n'
    '  writefs.close(rw)\n'
    'end\n')
PY
truncate -s 64M "$work/esp.img"
mkfs.fat -F 16 -n WFESP "$work/esp.img" > "$work/mkfs.log" 2>&1
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
for file in payload.bin short.txt gap.bin huge.bin read.lua close.lua write.lua supported.lua bind.ini; do mcopy -i "$work/esp.img" "$work/$file" "::/$file"; done
for file in "$work"/bind-*.lua; do mcopy -i "$work/esp.img" "$file" "::/$(basename "$file")"; done
truncate -s 32M "$work/fat.img"
mkfs.fat -F 16 -n WFFAT "$work/fat.img" >> "$work/mkfs.log" 2>&1
truncate -s 64M "$work/exfat.img"
mkfs.exfat -L WFEXFAT "$work/exfat.img" >> "$work/mkfs.log" 2>&1
truncate -s 128M "$work/ntfs.img"
mkntfs -F -Q -L WFNTFS "$work/ntfs.img" >> "$work/mkfs.log" 2>&1
cp "$work/ntfs.img" "$work/hiber.img"
ntfslabel "$work/hiber.img" WFHIBER > "$work/label.log" 2>&1
ntfscp -f "$work/hiber.img" "$work/hiberfil.sys" /hiberfil.sys > "$work/hiber.log" 2>&1
sha256sum "$work/hiber.img" | cut -d ' ' -f 1 > "$work/hiber.before"
# Put the main NTFS volume inside a GPT partition; retain boundary evidence.
truncate -s 132M "$work/ntfs-disk.img"
printf 'label: gpt\nstart=2048, size=262144, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\n' | sfdisk "$work/ntfs-disk.img" > "$work/partition.log" 2>&1
dd if="$work/ntfs.img" of="$work/ntfs-disk.img" bs=1M seek=1 conv=notrunc status=none
python3 - "$work" <<'PYBOUND'
from pathlib import Path
import sys
p = Path(sys.argv[1])
with (p/'ntfs-disk.img').open('rb') as f:
    p.joinpath('partition-prefix').write_bytes(f.read(1024 * 1024))
    f.seek(129 * 1024 * 1024)
    p.joinpath('partition-suffix').write_bytes(f.read())
PYBOUND
truncate -s 64M "$work/ntfs4k.img"
mkntfs -F -Q -s 4096 -L WFNTFS4K "$work/ntfs4k.img" >> "$work/mkfs.log" 2>&1
cp "$work/ntfs.img" "$work/readonly.img"
ntfslabel "$work/readonly.img" WFREADONLY >> "$work/label.log" 2>&1
cp "$work/ntfs.img" "$work/dirty.img"
ntfslabel "$work/dirty.img" WFDIRTY >> "$work/label.log" 2>&1
python3 - "$work/dirty.img" <<'PYDIRTY'
import ctypes as c
import ctypes.util
import sys
lib = c.CDLL(ctypes.util.find_library('ntfs-3g'), use_errno=True)
lib.ntfs_mount.argtypes = [c.c_char_p, c.c_ulong]
lib.ntfs_mount.restype = c.c_void_p
lib.ntfs_volume_write_flags.argtypes = [c.c_void_p, c.c_uint16]
lib.ntfs_umount.argtypes = [c.c_void_p, c.c_int]
vol = lib.ntfs_mount(sys.argv[1].encode(), 0x08000000)
assert vol, c.get_errno()
assert lib.ntfs_volume_write_flags(vol, int.from_bytes(b'\x01\x00', sys.byteorder)) == 0
assert lib.ntfs_umount(vol, 0) == 0
PYDIRTY
sha256sum "$work/dirty.img" | cut -d ' ' -f 1 > "$work/dirty.before"
cp "$work/ntfs.img" "$work/badupcase.img"
ntfslabel "$work/badupcase.img" WFBADUPCASE >> "$work/label.log" 2>&1
ntfscp -f -i "$work/badupcase.img" "$work/invalid-upcase" 10 > "$work/badupcase.log" 2>&1
sha256sum "$work/badupcase.img" | cut -d ' ' -f 1 > "$work/badupcase.before"
truncate -s 4M "$work/ntfsfull.img"
mkntfs -F -Q -L WFNTFSFULL "$work/ntfsfull.img" >> "$work/mkfs.log" 2>&1
# A nearly full FAT volume makes a real copy short-write, not a mocked callback.
truncate -s 4M "$work/full.img"
mkfs.fat -F 12 -n WFFULL "$work/full.img" >> "$work/mkfs.log" 2>&1
python3 - "$work/filler" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(bytes(4000 * 1024))
PY
mcopy -i "$work/full.img" "$work/filler" ::/filler
python3 - "$work/grub.cfg" <<'PY'
from pathlib import Path
import sys
lines = ['serial --speed=115200', 'terminal_input serial', 'terminal_output serial', 'set pager=0', 'echo WF_BEGIN']
def ok(command, name):
    lines.append(f'if {command}; then echo WF_OK_{name}; else echo WF_FAIL_{name}; fi')
def bad(command, name):
    lines.append(f'if {command}; then echo WF_FAIL_{name}; else echo WF_OK_{name}; fi')
ok('mount status', 'CORE_AVAILABLE')
ok('mount filesystems', 'FILESYSTEMS_CLI')
ok('insmod fat', 'FAT_READER')
ok('insmod ntfs', 'NTFS_READER')
ok('insmod exfat', 'EXFAT_READER')
for label, var in [('WFESP','esp'),('WFFAT','fat'),('WFEXFAT','exfat'),('WFNTFS','ntfs'),('WFFULL','full'),('WFNTFS4K','ntfs4k'),('WFHIBER','hiber'),('WFDIRTY','dirty'),('WFREADONLY','readonly'),('WFNTFSFULL','ntfsfull'),('WFBADUPCASE','badupcase')]:
    ok(f'search --no-floppy --label --set={var} {label}', f'FIND_{var}')
ok('insmod getkey', 'BIND_INPUT_PRELOAD')
ok('lua ($esp)/supported.lua', 'FILESYSTEMS_LUA')
ok('lua ($esp)/bind-input.lua', 'BIND_INPUT_CALL')
ok('lua ($esp)/bind-before.lua', 'BIND_BEFORE')
ok('insmod conv', 'BIND_CONV_LOAD')
ok('lua ($esp)/bind-conv.lua', 'BIND_CONV_CALL')
ok('rmmod conv', 'BIND_CONV_UNLOAD')
ok('lua ($esp)/bind-stale-conv.lua', 'BIND_CONV_STALE')
ok('insmod conv', 'BIND_CONV_RELOAD')
ok('lua ($esp)/bind-reload-conv.lua', 'BIND_CONV_FRESH')
ok('insmod ini', 'BIND_INI_LOAD')
ok('lua ($esp)/bind-ini-open.lua', 'BIND_INI_OPEN')
bad('rmmod ini', 'BIND_INI_PINNED_BLOCKS_UNLOAD')
ok('lua ($esp)/bind-ini-pinned.lua', 'BIND_INI_PIN_VALID')
ok('lua ($esp)/bind-ini-close.lua', 'BIND_INI_CLOSE')
ok('rmmod ini', 'BIND_INI_UNLOAD')
ok('lua ($esp)/bind-ini-stale.lua', 'BIND_INI_STALE')
ok('insmod ini', 'BIND_INI_RELOAD')
ok('lua ($esp)/bind-ini-reload.lua', 'BIND_INI_FRESH')
ok('rmmod lua', 'BIND_LUA_UNLOAD')
ok('insmod lua', 'BIND_LUA_RELOAD')
ok('lua ($esp)/bind-runtime-reload.lua', 'BIND_LUA_REATTACH')
ok('insmod wimboot', 'BIND_WIM_LOAD')
ok('lua ($esp)/bind-wim.lua', 'BIND_WIM_CALL')
ok('rmmod wimboot', 'BIND_WIM_UNLOAD')
ok('mount ($fat) 1', 'MOUNT_FAT')
ok('mount ($ntfs) 2', 'MOUNT_NTFS')
ok('mount ($exfat) 3', 'MOUNT_EXFAT')
ok('mount ($full) 4', 'MOUNT_FULL')
ok('mount ($ntfs4k) 6', 'MOUNT_NTFS4K')
lines.append('mount status')
bad('mount ($ntfs) 5', 'DUPLICATE_VOLUME')
bad('mount ($fat) 2', 'DUPLICATE_SLOT')
bad('mount ($esp) 0', 'SLOT_ZERO')
bad('touch 9:/unmounted', 'UNMOUNTED')
# hibernation fixture is the fifth data disk, separate from the clean label lookup.
bad('mount ($hiber) 5', 'HIBERNATION')
bad('mount ($dirty) 5', 'DIRTY')
bad('mount ($badupcase) 5', 'INVALID_UPCASE')
bad('mount ($badupcase) 5', 'INVALID_UPCASE_REPEAT')
ok('mount ($readonly) 7', 'MOUNT_READONLY_TARGET')
ok('mount ($ntfsfull) 8', 'MOUNT_NTFSFULL')
for num, kind in [(1,'FAT'),(2,'NTFS'),(3,'EXFAT')]:
    ok(f'mkdir {num}:/test', f'{kind}_MKDIR')
    ok(f'cp ($esp)/payload.bin {num}:/test/big.bin', f'{kind}_COPY_BIG')
    ok(f'cp ($esp)/short.txt {num}:/test/gap.bin', f'{kind}_COPY_SMALL')
    ok(f'write_file {num}:/test/gap.bin END 4096', f'{kind}_WRITE_GAP')
    ok(f'touch {num}:/test/empty 2024 2 29 12 34 56', f'{kind}_TOUCH')
    bad(f'touch {num}:/test/invalid 2023 2 29', f'{kind}_BAD_DATE')
    bad(f'rm {num}:/test', f'{kind}_NONEMPTY')
    bad(f'cp {num}:/test/big.bin {num}:/test/BIG.BIN', f'{kind}_SELF_COPY')
    ok(f'rename {num}:/test/empty {num}:/test/renamed', f'{kind}_RENAME')
    ok(f'mkdir {num}:/other', f'{kind}_OTHER')
    ok(f'mv {num}:/test/renamed {num}:/other/moved', f'{kind}_SAME_MOVE')
    ok(f'rm {num}:/other/moved', f'{kind}_REMOVE_FILE')
    ok(f'rm {num}:/other', f'{kind}_REMOVE_DIR')
ok('lua ($esp)/read.lua', 'LUA_WRITEFS_READ')
bad('umount 1', 'LUA_OPEN_PREVENTS_UNMOUNT')
ok('lua ($esp)/close.lua', 'LUA_WRITEFS_CLOSE')
ok('lua ($esp)/write.lua', 'LUA_WRITEFS_MULTIFS')
ok('cp 1:/test/big.bin 2:/from-fat.bin', 'FAT_TO_NTFS')
ok('mkdir 2:/中文目录', 'UNICODE_DIR')
ok('cp ($esp)/payload.bin 2:/中文目录/文件.bin', 'UNICODE_COPY')
ok('mkdir 2:/tree', 'TREE_ROOT')
ok('mkdir 2:/tree/child', 'TREE_CHILD')
ok('cp ($esp)/short.txt 2:/tree/child/data', 'TREE_FILE')
ok('cp 2:/tree/child/data 1:/tree-before', 'TREE_CACHE_SEED')
ok('mv 2:/TREE 2:/moved-tree', 'TREE_RENAME')
bad('cp 2:/tree/child/data 1:/stale-tree', 'TREE_OLD_PATH_GONE')
ok('cp 2:/moved-tree/child/data 1:/tree-after', 'TREE_NEW_PATH_READ')
ok('cp ($esp)/payload.bin 6:/large.bin', 'NTFS4K_COPY')
ok('touch 6:/gap.bin', 'NTFS4K_TOUCH')
ok('write_file 6:/gap.bin END 4096', 'NTFS4K_WRITE')
ok('umount 6', 'NTFS4K_UNMOUNT')
ok('cmp ($esp)/payload.bin ($ntfs4k)/large.bin', 'NTFS4K_CONTENT')
ok('cp 2:/test/big.bin 3:/from-ntfs.bin', 'NTFS_TO_EXFAT')
ok('cp 3:/test/big.bin 1:/from-exfat.bin', 'EXFAT_TO_FAT')
ok('cp 2:/test/big.bin 2:/overwrite.bin', 'NTFS_SAME_COPY')
ok('cp ($esp)/short.txt 2:/overwrite.bin', 'NTFS_TRUNCATE')
ok('cp ($esp)/short.txt 1:/move-source', 'MOVE_SEED')
bad('mv 1:/move-source 7:/readonly-target', 'READONLY_MOVE_FAILS')
ok('cp 1:/move-source 2:/readonly-source-survived', 'READONLY_SOURCE_SURVIVES')
bad('umount 7', 'READONLY_UNMOUNT_REPORTS_ERROR')
ok('cp ($esp)/huge.bin 1:/huge-source', 'NTFSFULL_SOURCE_SEED')
bad('mv 1:/huge-source 8:/huge-target', 'NTFSFULL_MOVE_FAILS')
ok('cp 1:/huge-source 2:/ntfsfull-source-survived', 'NTFSFULL_SOURCE_SURVIVES')
ok('umount 8', 'NTFSFULL_UNMOUNT')
ok('mv 1:/move-source 2:/moved-cross', 'CROSS_MOVE')
bad('write_file 1:/move-source missing', 'MOVE_REMOVED_SOURCE')
bad('mv 2:/test/big.bin 4:/too-big.bin', 'FULL_MOVE_FAILS')
ok('cp 2:/test/big.bin 1:/source-survived.bin', 'FULL_MOVE_SOURCE_SURVIVES')
bad('rename 1:/test/gap.bin 2:/wrong', 'CROSS_RENAME')
bad('mv 2:/test 2:/test/child', 'MOVE_INTO_SELF')
for num, kind in [(1,'FAT'),(2,'NTFS'),(3,'EXFAT'),(4,'FULL')]:
    ok(f'umount {num}', f'{kind}_UNMOUNT')
ok('cmp ($esp)/payload.bin ($fat)/test/big.bin', 'FAT_BIG_CONTENT')
ok('cmp ($esp)/payload.bin ($ntfs)/test/big.bin', 'NTFS_BIG_CONTENT')
ok('cmp ($esp)/payload.bin ($exfat)/test/big.bin', 'EXFAT_BIG_CONTENT')
for var in ['fat','ntfs','exfat']:
    ok(f'cmp ($esp)/gap.bin (${var})/test/gap.bin', f'{var}_GAP_CONTENT')
for var, path in [('ntfs','from-fat.bin'),('exfat','from-ntfs.bin'),('fat','from-exfat.bin'),('fat','source-survived.bin')]:
    ok(f'cmp ($esp)/payload.bin (${var})/{path}', f'{var}_{path.replace("-","_").replace(".","_")}')
ok('cmp ($esp)/short.txt ($ntfs)/overwrite.bin', 'NTFS_TRUNCATED_CONTENT')
ok('cmp ($esp)/short.txt ($ntfs)/moved-cross', 'CROSS_MOVE_CONTENT')
ok('cmp ($esp)/huge.bin ($ntfs)/ntfsfull-source-survived', 'NTFSFULL_SOURCE_CONTENT')
ok('cmp ($esp)/short.txt ($ntfs)/moved-tree/child/data', 'MOVED_TREE_CONTENT')
ok('cmp ($esp)/payload.bin ($ntfs)/中文目录/文件.bin', 'UNICODE_CONTENT')
ok('rmmod ntfs3g', 'NTFS_UNLOAD')
ok('mount ($ntfs) 2', 'NTFS_REMOUNT')
ok('touch 2:/after-remount', 'NTFS_AFTER_REMOUNT')
ok('umount 2', 'NTFS_REUNMOUNT')
lines.extend(['echo WF_DONE','halt'])
Path(sys.argv[1]).write_text('\n'.join(lines)+'\n')
PY
mods='serial terminal normal echo test halt fat exfat ntfs ntfscomp part_msdos part_gpt search search_label cmp fatfs ntfs3g lua conv ini getkey wimboot minicmd'
case "$platform" in
  x86_64-efi)
    "$build/grub-mkstandalone" -O x86_64-efi -d "$build/grub-core" \
      -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
      --install-modules="$mods" --modules='serial terminal normal echo test halt fat part_msdos part_gpt search search_label cmp' \
      "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1
    mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
    cp "$vars" "$work/vars.fd"
    logical_sector=4096
    set -- -drive "if=pflash,format=raw,readonly=on,file=$code" -drive "if=pflash,format=raw,file=$work/vars.fd"
    ;;
  i386-pc)
    mkdir -p "$work/iso/boot/grub"
    cp "$work/grub.cfg" "$work/iso/boot/grub/grub.cfg"
    "$build/grub-mkrescue" -d "$build/grub-core" -o "$work/boot.iso" "$work/iso" > "$work/pack.log" 2>&1
    logical_sector=512
    set -- -cdrom "$work/boot.iso" -boot d
    ;;
  *) fail "unsupported test platform $platform" ;;
esac
timeout 95s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 -m 512M \
  -display none -monitor none -no-reboot -serial "file:$work/serial.log" "$@" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  -drive "if=virtio,format=raw,file=$work/fat.img" \
  -drive "if=virtio,format=raw,file=$work/ntfs-disk.img" \
  -drive "if=virtio,format=raw,file=$work/exfat.img" \
  -drive "if=virtio,format=raw,file=$work/hiber.img" \
  -drive "if=virtio,format=raw,file=$work/full.img" \
  -drive "if=none,id=ntfs4k,format=raw,file=$work/ntfs4k.img" \
  -device virtio-blk-pci,drive=ntfs4k,logical_block_size=$logical_sector,physical_block_size=4096 \
  -drive "if=virtio,format=raw,readonly=on,file=$work/readonly.img" \
  -drive "if=virtio,format=raw,file=$work/dirty.img" \
  -drive "if=virtio,format=raw,file=$work/ntfsfull.img" \
  -drive "if=virtio,format=raw,file=$work/badupcase.img" \
  > "$work/qemu.log" 2>&1 || fail 'QEMU did not exit cleanly'
python3 - "$work" <<'PYBOUND'
from pathlib import Path
import sys
p = Path(sys.argv[1])
with (p/'ntfs-disk.img').open('rb') as f:
    assert f.read(1024 * 1024) == p.joinpath('partition-prefix').read_bytes()
    p.joinpath('ntfs.img').write_bytes(f.read(128 * 1024 * 1024))
    assert f.read() == p.joinpath('partition-suffix').read_bytes()
PYBOUND
python3 - "$work" <<'PY'
from pathlib import Path
import re, sys
p = Path(sys.argv[1])
log = (p/'serial.log').read_text(errors='replace')
expected = re.findall(r'echo (WF_OK_\w+)', (p/'grub.cfg').read_text())
assert 'WF_BEGIN' in log and 'WF_DONE' in log, log
assert 'WF_FAIL_' not in log, log
for marker in expected:
    assert marker in log, marker
assert '#PF' not in log and 'Unhandled exception' not in log, log
print(f'PASS: {len(expected)} QEMU writefs assertions')
PY
ntfscat "$work/ntfs.img" /test/big.bin > "$work/ntfs-big.bin"
cmp "$work/payload.bin" "$work/ntfs-big.bin"
ntfscat "$work/ntfs.img" /test/gap.bin > "$work/ntfs-gap.bin"
cmp "$work/gap.bin" "$work/ntfs-gap.bin"
ntfscat "$work/ntfs.img" /overwrite.bin > "$work/ntfs-short.txt"
cmp "$work/short.txt" "$work/ntfs-short.txt"
fsck.fat -n "$work/fat.img" > "$work/fat-check.log" 2>&1
fsck.exfat -n "$work/exfat.img" > "$work/exfat-check.log" 2>&1
ntfsfix -n "$work/ntfs.img" > "$work/ntfs-check.log" 2>&1
sha256sum "$work/hiber.img" | cut -d ' ' -f 1 > "$work/hiber.after"
cmp "$work/hiber.before" "$work/hiber.after"
sha256sum "$work/dirty.img" | cut -d ' ' -f 1 > "$work/dirty.after"
cmp "$work/dirty.before" "$work/dirty.after"
sha256sum "$work/badupcase.img" | cut -d ' ' -f 1 > "$work/badupcase.after"
cmp "$work/badupcase.before" "$work/badupcase.after"
ntfsfix -n "$work/ntfsfull.img" > "$work/ntfsfull-check.log" 2>&1
passed=1
echo 'PASS: independent host readback, FAT/exFAT checks, NTFS metadata check, hibernated image unchanged'
