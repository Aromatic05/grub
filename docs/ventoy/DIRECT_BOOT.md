# Direct Ventoy boot from GRUB

The public GRUB command is independent of menu implementations:

```grub
insmod ventoy
ventoy (hd0,gpt2)/ISO/archlinux.iso
ventoy (hd0,gpt2)/ISO/winpe.iso
```

On x86_64 UEFI the module recognizes Linux/Windows ISO boot content, selects
the corresponding already-ported Ventoy chain producer, locates physical
file extents and the original runtime, and invokes the same EFI Consumer.
The command performs the final GRUB boot; it does not merely prepare data.
If a Consumer returns to GRUB, the command reports an error.

**No GRUBFM integration or dependency is involved.** Command-line GRUB,
a generic `grub.cfg`, and any menu capable of invoking an ordinary GRUB
command all call the same API. No special frontend hooks are required.

## Boot behavior

- **Linux ISO:** `vt_img_sector`, Ventoy Linux initrd/CPIO discovery and
  replacement, `vt_linux_chain_data`. The consumer receives `isoefi=on`,
  the architecture-appropriate `FirstTry` and `iso_fat`. This is unchanged
  from the independently verified Linux implementation.
- **Windows ISO:** `vt_windows_reset`, image extents, UDF/ISO9660 probe,
  BCD-based WIM discovery (with `boot.wim` fallback), UDF-aware WIM mapping,
  and `vt_windows_chain_data`. The original `vtoyjump64.exe` is injected
  by the existing Windows WIM engine. The EFI consumer receives the
  Windows-specific `isoefi=<LoadIsoEfiDriver>`, `iso_udf` or `iso_fat`,
  and `mem:...` arguments. In particular, **do not force `isoefi=on` or
  `FirstTry` for Windows**.
- **Shared handoff:** both invoke `vt_acpi_param`, the existing GRUB
  `chainloader`, and `boot` with the generated chain's environment values.
  Neither the EFI Consumer nor GRUB's general boot infrastructure was forked.

The image must be a physically accessible, partition-backed ISO with a
GRUB device in its path, e.g. `(hd1,msdos1)/ISO/winpe.iso`. Generic
in-memory loopback ISO files are not yet a substitute for the consumer's
physical extent mapping. The internal `loop` and `vtoyprobe` device
names must not already be occupied.

## Runtime resolution

The module first checks the image's device for `/ventoy`, then searches
other GRUB-visible devices. It can also be explicitly directed to a
single consistent runtime version:

```grub
ventoy --runtime=(hd0,gpt1)/ventoy (hd1,gpt2)/ISO/winpe.iso
# Or configure a default:
set ventoy_runtime=(hd0,gpt1)/ventoy
ventoy (hd1,gpt2)/ISO/winpe.iso

# Check image type, readable extents and runtime presence, without booting:
ventoy --check (hd1,gpt2)/ISO/winpe.iso
```

Linux x64 runtime requires the matching original `ventoy.cpio`,
`ventoy_x86.cpio`, `ventoy_x64.efi`, and `iso9660_x64.efi` (plus
any other files used by the selected Linux distribution).

Windows x64 runtime requires matching `ventoy_x64.efi`, `udf_x64.efi`,
`iso9660_x64.efi`, and `vtoyjump64.exe`. Do not mix incompatible EFI
Consumer/Jump binaries. Absence or failed reading of the Jump executable
during a required WIM patch is an explicit error rather than an accepted
incomplete chain.

## Verified scenarios and limits

- Arch Linux 2026.03.01 x86_64 UEFI: EFI menu, Linux kernel, initramfs,
  and WinPE-independent earlier QEMU root automatic-login acceptance.
- Windows PE ISO x86_64 UEFI: public `ventoy PATH` directly boots the
  Windows PE GUI. **Inside Windows**, the original `vtoyjump64.exe`
  successfully opens `C:\ISO\winpe.iso`, mounts it via the Windows
  virtual disk APIs, and exposes the matching UDF DVD volume.
  The verified chain has size 1,667,736 bytes, 4 Override chunks,
  460,988,416 virtual image bytes and UDF flag 1.
- The original `ventoy_x64.efi` is unchanged. On nonstandard partition
  layouts it can still display the legacy false "not a standard Ventoy
  device" warning and delay before continuing. Never spoof VLNK fields
  to bypass it.
- BIOS one-command Windows boot, other Windows ISO variants, Windows WIM as
  a standalone argument, VHD(x), IMG, VTOY and other boot paths have not
  passed this new command's end-to-end acceptance; they are not claimed.
- UDF WIM handling and initial EFI handoff do **not** by themselves
  establish Windows user-space success. Observe the actual guest.
- Existing unload/lifecycle behavior is intentionally unchanged.

## Isolated QEMU tests

```sh
# Linux CLI/consumer:
tests/ventoy_boot_qemu.sh --check
tests/ventoy_boot_qemu.sh --boot

# The previously proven internal Windows producer/EFI path:
tests/ventoy_consumer_qemu.sh --prepare
tests/ventoy_consumer_qemu.sh --trace

# The public Windows command (no manual vt_* steps in its grub.cfg):
tests/ventoy_consumer_qemu.sh --direct-check
VENTOY_TEST_KEEP=1 VENTOY_SCREENSHOT=1 VENTOY_CAPTURE_AFTER=155 \
  tests/ventoy_consumer_qemu.sh --direct-boot
```

The WinPE test uses SATA/IDE guest disks with disposable writable QCOW2
overlays backed by unchanged RAW fixtures. The built EFI Consumer and Jump
are pinned to their historical SHA256 values. The script checks deterministic
GRUB/chain fields, but **does not automatically parse the guest's Windows
mount log**. Timeout/nonreturn of an EFI app is not an OS-boot pass; the
direct-boot test returns an UNVERIFIED status until the captured Windows
screenshots are inspected.

Full public-command GUI and in-Windows ISO mount acceptance artifacts:
`../out/testenv/work/winpe-direct-verified-20261009/`.
Historical comparison and failed VirtIO counterexample:
`docs/ventoy/CONSUMER_REGRESSION.md`.
