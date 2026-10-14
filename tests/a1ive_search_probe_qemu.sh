#!/bin/sh
# Isolated EFI QEMU regression for the a1ive search/probe extensions:
#   search --part-label/-L, --part-uuid/-g, --disk-uuid/-U
#   search.part_label / search.part_uuid / search.disk_uuid commands
#   probe --partuuid/-g, probe --part-uuid (new), probe --bootable/-b
#   --set variables and not-found statuses
#
# Layout:
#   - a standalone FAT EFI boot disk holds BOOTX64.EFI (this test never touches
#     the repository build, physical disks, or any other source file);
#   - a synthetic GPT disk is created with sgdisk (fixed disk GUID, fixed
#     partition GUIDs, UTF-16 partition names) and partition 1 is formatted
#     in place with mkfs.vfat --offset, then a marker is written with mtools
#     at the same byte offset.
#
# The test only writes under a mktemp directory.  It is executed by the
# integrator; this file does not run QEMU on the author's behalf.
set -eu

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
efi_build=${A1C_EFI_BUILD:-"$repo/build-x86_64-efi"}
ovmf_code=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}

# Fixed identities baked into both the fixtures and the guest assertions.
disk_guid=a1c00000-0000-4000-8000-0000000000ff
esp_guid=a1c00001-0000-4000-8000-000000000001
data_guid=a1c00002-0000-4000-8000-000000000002
esp_label=A1CESP
data_label=A1CDATA
esp_fslabel=A1CESPFS

for tool in qemu-system-x86_64 sgdisk sfdisk mkfs.vfat mmd mcopy truncate timeout python3; do
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

work=$(mktemp -d "${TMPDIR:-/tmp}/a1ive-search-probe.XXXXXX")
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

fail()
{
  echo "FAIL: $*" >&2
  exit 1
}

# ----------------------------------------------------------------- fixtures

# Synthetic GPT disk: ESP-type partition 1 (bootable) + non-ESP partition 2.
truncate -s 64M "$work/gpt.img"
if ! sgdisk --clear \
  --disk-guid="$disk_guid" \
  --new=1:2048:40959 --typecode=1:ef00 --partition-guid=1:"$esp_guid" \
  --change-name=1:"$esp_label" \
  --new=2:40960:61439 --typecode=2:8300 --partition-guid=2:"$data_guid" \
  --change-name=2:"$data_label" \
  "$work/gpt.img" > "$work/sgdisk.log" 2>&1; then
  cat "$work/sgdisk.log" >&2
  fail "sgdisk could not build the synthetic GPT disk"
fi

# Format ONLY partition 1 (LBAs 2048..40959, 38912 sectors), not the
# remainder of the disk containing partition 2 and the backup GPT.
mkfs.vfat -F 16 -n "$esp_fslabel" --offset=2048 "$work/gpt.img" 19456 > "$work/mkfs.log" 2>&1 ||
  { cat "$work/mkfs.log" >&2; fail "mkfs.vfat --offset failed"; }
printf 'a1c-gpt-marker\n' > "$work/marker"
mcopy -i "$work/gpt.img@@1048576" "$work/marker" ::/a1c-gpt.marker >/dev/null 2>&1 ||
  fail "could not write the marker into the offset FAT filesystem"

# Separate MSDOS disk: an active primary partition plus an inactive one.
# Keep it independent of GPT, so probe -g and -b exercise the MBR path.
truncate -s 48M "$work/mbr.img"
if ! sfdisk "$work/mbr.img" > "$work/sfdisk.log" 2>&1 <<'SFDISK'
label: dos
label-id: 0x12345678
unit: sectors
start=2048,size=32768,type=c,bootable
start=34816,size=8192,type=83
SFDISK
then
  cat "$work/sfdisk.log" >&2
  fail "could not create active/inactive MBR partitions"
fi

# EFI boot disk: a separate plain FAT16 volume with the standalone image.
truncate -s 48M "$work/esp.img"
mkfs.vfat -F 16 -n A1CESPB "$work/esp.img" >/dev/null

# ------------------------------------------------------------- guest script
cat > "$work/grub.cfg" <<CFG
serial --speed=115200
terminal_input serial
terminal_output serial
set pager=0
echo A1C_SP_BEGIN

# Old insmod module names must load from standalone's embedded module store,
# not simply appear registered because every module was preloaded.
if insmod search_part_label; then echo A1C_OK_INSMOD_PL; else echo A1C_ERROR_INSMOD_PL; fi
if insmod search_part_uuid; then echo A1C_OK_INSMOD_PU; else echo A1C_ERROR_INSMOD_PU; fi
if insmod search_disk_uuid; then echo A1C_OK_INSMOD_DU; else echo A1C_ERROR_INSMOD_DU; fi
if insmod search; then echo A1C_OK_INSMOD_SEARCH; else echo A1C_ERROR_INSMOD_SEARCH; fi

# Baseline: the FAT created with mkfs.vfat --offset is readable and the GPT
# partition map is attached.
search --file --set=fd /a1c-gpt.marker
if [ -n "\$fd" ]; then
  probe --partmap -s fm "(\$fd)"
  if [ "\$fm" = "gpt" ]; then echo A1C_OK_FILE_GPT; else echo "A1C_ERROR_FILE_GPT=\$fd/\$fm"; fi
else
  echo "A1C_ERROR_FILE_GPT_UNSET=\$fd"
fi

# search --part-label (long) must find the ESP partition by its UTF-16 name.
search --part-label --set=pl $esp_label
if [ -n "\$pl" ]; then
  probe --partuuid -s pp "(\$pl)"
  if [ "\$pp" = "$esp_guid" ]; then echo A1C_OK_PL_LONG; else echo "A1C_ERROR_PL_LONG=\$pl/\$pp"; fi
else
  echo "A1C_ERROR_PL_LONG_UNSET=\$pl"
fi

# search -L (short) must find the non-ESP partition, proving discrimination.
search -L --set=pl2 $data_label
if [ -n "\$pl2" ]; then
  probe --partuuid -s pp2 "(\$pl2)"
  if [ "\$pp2" = "$data_guid" ]; then echo A1C_OK_PL_SHORT; else echo "A1C_ERROR_PL_SHORT=\$pl2/\$pp2"; fi
else
  echo "A1C_ERROR_PL_SHORT_UNSET=\$pl2"
fi

# search --part-uuid (long) together with probe -g (short).
search --part-uuid --set=pu $data_guid
if [ -n "\$pu" ]; then
  probe -g -s pg "(\$pu)"
  if [ "\$pg" = "$data_guid" ]; then echo A1C_OK_PU_LONG; else echo "A1C_ERROR_PU_LONG=\$pu/\$pg"; fi
else
  echo "A1C_ERROR_PU_LONG_UNSET=\$pu"
fi

# search -g (short) together with probe --partuuid (long).
search -g --set=pu2 $esp_guid
if [ -n "\$pu2" ]; then
  probe --partuuid -s pg2 "(\$pu2)"
  if [ "\$pg2" = "$esp_guid" ]; then echo A1C_OK_PU_SHORT; else echo "A1C_ERROR_PU_SHORT=\$pu2/\$pg2"; fi
else
  echo "A1C_ERROR_PU_SHORT_UNSET=\$pu2"
fi

# Legacy repeatable platform hint syntax and default --set root variable.
search --part-uuid --hint=(hd0) --hint-efi=(hd1,gpt1) --set=hinted $esp_guid
if [ "\$hinted" = "\$pu2" ]; then echo A1C_OK_HINT_EFI; else echo "A1C_ERROR_HINT_EFI=\$hinted/\$pu2"; fi
set original_root=\$root
search --part-label --set $esp_label
if [ "\$root" = "\$pl" ]; then echo A1C_OK_SET_ROOT_DEFAULT; else echo "A1C_ERROR_SET_ROOT_DEFAULT=\$root/\$pl"; fi
set root=\$original_root

# probe --part-uuid (new long-only option) reports the GPT partition GUID.
if [ -n "\$pu2" ]; then
  probe --part-uuid -s np "(\$pu2)"
  if [ "\$np" = "$esp_guid" ]; then echo A1C_OK_PROBE_PARTUUID_NEW; else echo "A1C_ERROR_PROBE_PARTUUID_NEW=\$np"; fi
  probe --fs -s fs "(\$pu2)"
  if [ "\$fs" = "fat" ]; then echo A1C_OK_FS_OFFSET; else echo "A1C_ERROR_FS_OFFSET=\$fs"; fi
  probe --label -s fl "(\$pu2)"
  if [ "\$fl" = "$esp_fslabel" ]; then echo A1C_OK_LABEL_OFFSET; else echo "A1C_ERROR_LABEL_OFFSET=\$fl"; fi
else
  echo "A1C_ERROR_PROBE_PARTUUID_NEW_UNSET=\$pu2"
fi

# search --disk-uuid (long): only the synthetic disk has a GPT header.
search --disk-uuid --set=du $disk_guid
if [ -n "\$du" ]; then echo A1C_OK_DU_LONG; else echo "A1C_ERROR_DU_LONG=\$du"; fi

# search -U (short) must resolve to exactly the same first device.
search -U --set=du2 $disk_guid
if [ -n "\$du2" ]; then
  if [ "\$du2" = "\$du" ]; then echo A1C_OK_DU_SHORT; else echo "A1C_ERROR_DU_SHORT=\$du2/\$du"; fi
else
  echo "A1C_ERROR_DU_SHORT_UNSET=\$du2"
fi

# Standalone command aliases.
search.part_label $esp_label al
if [ -n "\$al" ]; then
  probe --partuuid -s pa "(\$al)"
  if [ "\$pa" = "$esp_guid" ]; then echo A1C_OK_CMD_PL; else echo "A1C_ERROR_CMD_PL=\$al/\$pa"; fi
else
  echo "A1C_ERROR_CMD_PL_UNSET=\$al"
fi

search.part_uuid $data_guid au
if [ -n "\$au" ]; then echo A1C_OK_CMD_PU; else echo "A1C_ERROR_CMD_PU=\$au"; fi

search.disk_uuid $disk_guid ad
if [ -n "\$ad" ]; then echo A1C_OK_CMD_DU; else echo "A1C_ERROR_CMD_DU=\$ad"; fi

# probe --bootable/-b: EFI System type is bootable, non-ESP is not.
if [ -n "\$pu2" ]; then
  probe --bootable -s b1 "(\$pu2)"
  if [ "\$b1" = "bootable" ]; then echo A1C_OK_BOOT_ESP; else echo "A1C_ERROR_BOOT_ESP=\$b1"; fi
else
  echo "A1C_ERROR_BOOT_ESP_UNSET=\$pu2"
fi
if [ -n "\$pl2" ]; then
  probe -b -s b2 "(\$pl2)"
  if [ "\$b2" = "none" ]; then echo A1C_OK_BOOT_NONESP; else echo "A1C_ERROR_BOOT_NONESP=\$b2"; fi
else
  echo "A1C_ERROR_BOOT_NONESP_UNSET=\$pl2"
fi

# Legacy MSDOS MBR partition signature and bootable flag.
probe -g -s mbruuid (hd2,msdos1)
if [ "\$mbruuid" = "12345678-01" ]; then echo A1C_OK_MBR_PARTUUID; else echo "A1C_ERROR_MBR_PARTUUID=\$mbruuid"; fi
probe -b -s mbractive (hd2,msdos1)
if [ "\$mbractive" = "bootable" ]; then echo A1C_OK_MBR_ACTIVE; else echo "A1C_ERROR_MBR_ACTIVE=\$mbractive"; fi
probe --bootable -s mbrinactive (hd2,msdos2)
if [ "\$mbrinactive" = "none" ]; then echo A1C_OK_MBR_INACTIVE; else echo "A1C_ERROR_MBR_INACTIVE=\$mbrinactive"; fi

# a1ive -g on an unpartitioned device yields an empty string, whereas
# Aromatic's newer --part-uuid retains its existing 'none' result.
probe -g -s old_whole (hd2)
if [ -z "\$old_whole" ]; then echo A1C_OK_MBR_WHOLE_LEGACY; else echo "A1C_ERROR_MBR_WHOLE_LEGACY=\$old_whole"; fi
probe --part-uuid -s new_whole (hd2)
if [ "\$new_whole" = "none" ]; then echo A1C_OK_MBR_WHOLE_NEW; else echo "A1C_ERROR_MBR_WHOLE_NEW=\$new_whole"; fi

# --set variables are populated; not-found searches fail and leave the
# variable empty (--quiet suppresses the no-match diagnostic, but GRUB may
# still report I/O errors from unrelated non-matching devices).
if search --part-uuid --quiet --set=nf1 00000000-0000-0000-0000-000000000000; then
  echo A1C_ERROR_NF_PARTUUID_ACCEPTED
else
  echo A1C_OK_NF_PARTUUID
fi
if [ -z "\$nf1" ]; then echo A1C_OK_NF_PARTUUID_EMPTY; else echo "A1C_ERROR_NF_PARTUUID_SET=\$nf1"; fi

if search --part-label --quiet --set=nf2 A1CNOSUCHLABEL; then
  echo A1C_ERROR_NF_PARTLABEL_ACCEPTED
else
  echo A1C_OK_NF_PARTLABEL
fi
if [ -z "\$nf2" ]; then echo A1C_OK_NF_PARTLABEL_EMPTY; else echo "A1C_ERROR_NF_PARTLABEL_SET=\$nf2"; fi

if search --disk-uuid --quiet --set=nf3 00000000-0000-0000-0000-0000000000ff; then
  echo A1C_ERROR_NF_DISKUUID_ACCEPTED
else
  echo A1C_OK_NF_DISKUUID
fi
if [ -z "\$nf3" ]; then echo A1C_OK_NF_DISKUUID_EMPTY; else echo "A1C_ERROR_NF_DISKUUID_SET=\$nf3"; fi

echo A1C_SP_DONE
halt
CFG

# ------------------------------------------------------------ boot image
mods='serial terminal normal echo test search search_fs_file search_fs_uuid
  search_label search_part_label search_part_uuid search_disk_uuid probe gpt
  part_gpt fat part_msdos halt'
preloaded='serial terminal normal echo test probe part_gpt fat part_msdos halt'

if ! "$efi_build/grub-mkstandalone" -O x86_64-efi -d "$efi_build/grub-core" \
  -o "$work/BOOTX64.EFI" --fonts='' --locales='' --themes='' --compress=no \
  --install-modules="$mods" --modules="$preloaded" \
  "boot/grub/grub.cfg=$work/grub.cfg" > "$work/pack.log" 2>&1; then
  tail -n 8 "$work/pack.log" >&2
  fail "grub-mkstandalone could not build the image (search_part_* / probe port not in $efi_build?)"
fi

mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$work/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
cp "$ovmf_vars" "$work/vars.fd"

# ------------------------------------------------------------------- QEMU
if ! timeout 90s qemu-system-x86_64 -machine q35,accel=tcg -cpu qemu64 \
  -m 512M -display none -no-reboot \
  -serial "file:$work/serial.log" \
  -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,file=$work/vars.fd" \
  -drive "if=virtio,format=raw,file=$work/esp.img" \
  -drive "if=virtio,format=raw,file=$work/gpt.img" \
  -drive "if=virtio,format=raw,file=$work/mbr.img" \
  > "$work/qemu.log" 2>&1; then
  fail "QEMU did not exit cleanly (see $work/qemu.log, $work/serial.log)"
fi

# -------------------------------------------------------------- assertions
log="$work/serial.log"
grep -aq A1C_SP_BEGIN "$log" || fail "guest never started"
grep -aq A1C_SP_DONE "$log" || fail "guest script did not finish"

for m in A1C_OK_INSMOD_PL A1C_OK_INSMOD_PU A1C_OK_INSMOD_DU A1C_OK_INSMOD_SEARCH \
         A1C_OK_FILE_GPT \
         A1C_OK_PL_LONG A1C_OK_PL_SHORT \
         A1C_OK_PU_LONG A1C_OK_PU_SHORT \
         A1C_OK_HINT_EFI A1C_OK_SET_ROOT_DEFAULT \
         A1C_OK_PROBE_PARTUUID_NEW A1C_OK_FS_OFFSET A1C_OK_LABEL_OFFSET \
         A1C_OK_DU_LONG A1C_OK_DU_SHORT \
         A1C_OK_CMD_PL A1C_OK_CMD_PU A1C_OK_CMD_DU \
         A1C_OK_BOOT_ESP A1C_OK_BOOT_NONESP \
         A1C_OK_MBR_PARTUUID A1C_OK_MBR_ACTIVE A1C_OK_MBR_INACTIVE \
         A1C_OK_MBR_WHOLE_LEGACY A1C_OK_MBR_WHOLE_NEW \
         A1C_OK_NF_PARTUUID A1C_OK_NF_PARTUUID_EMPTY \
         A1C_OK_NF_PARTLABEL A1C_OK_NF_PARTLABEL_EMPTY \
         A1C_OK_NF_DISKUUID A1C_OK_NF_DISKUUID_EMPTY; do
  grep -aq "$m" "$log" || fail "missing marker $m"
done

if grep -aq 'A1C_ERROR_' "$log"; then
  grep -a 'A1C_ERROR_' "$log" >&2
  fail "a search/probe assertion reported an error marker"
fi
# Positive searches must not emit errors.  During deliberately unsuccessful
# searches GRUB may report an unrelated no-media/procfs/GPT probe error from
# its device iterator, even when --quiet suppresses the "no such device"
# diagnostic.  This is pre-existing generic error handling, not a match.
if sed -n '/A1C_SP_BEGIN/,/A1C_OK_MBR_WHOLE_NEW/p' "$log" | grep -aq 'error:'; then
  fail "unexpected GRUB error during successful search/probe"
fi
if grep -aq 'no such device:' "$log"; then
  fail "search --quiet printed a no-match diagnostic"
fi

passed=1
echo "PASS: search part-label/-L part-uuid/-g disk-uuid/-U, search.part_* aliases, probe -g/--part-uuid/-b, --set and not-found"
