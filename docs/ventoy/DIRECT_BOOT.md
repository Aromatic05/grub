# Direct Ventoy boot from GRUB

The port provides one public command for booting an image without using
Ventoy's original multi-partition boot menu:

```grub
insmod ventoy
ventoy (hd0,gpt2)/ISO/archlinux.iso
```

On the currently implemented EFI path, the command validates the ISO,
locates the original Ventoy runtime, gathers physical file extents, prepares
the Linux boot-chain metadata and CPIO, and chainloads the EFI consumer.

The command does **not** merely prepare data. It attempts the final GRUB
`boot` operation; if control returns to GRUB, it reports an error.

## Runtime files

The directory must contain the matching platform's
`ventoy.cpio`, `ventoy_x86.cpio` (or corresponding architecture archive),
`ventoy_x64.efi` (or corresponding EFI executable), and, on x64,
`iso9660_x64.efi`. It need not be on the image's filesystem or on a
Ventoy-formatted drive.

The command first tries `(<image-device>)/ventoy`, then finds compatible
runtime directories across GRUB disks/partitions. Override discovery
explicitly for reproducibility:

```grub
set ventoy_runtime=(hd0,gpt1)/ventoy
ventoy (hd1,gpt2)/images/rescue.iso

# Or just for this invocation:
ventoy --runtime=(hd0,gpt1)/ventoy (hd1,gpt2)/images/rescue.iso
```

Preflight without creating boot-chain data:

```grub
ventoy --check (hd1,gpt2)/images/rescue.iso
```

The path must include the GRUB device, for example `(hd0,gpt2)/ISO/a.iso`.
No Linux device paths are accepted.

## GRUBFM

The templates at `extras/grubfm/boot/grubfm` include:

- `types/iso`: an ISO file type with a “Boot with Ventoy” menu item
- `rules/ventoy.cfg`: a two-command rule that calls `ventoy "$grubfm_file"`

Install/merge these entries under `/boot/grubfm` on the device configured
as GRUBFM's root (do not blindly replace existing `types/iso` rules).
For example, when templates are present on `(hd0,gpt1)`:

```grub
insmod grubfm
grubfm_set -r hd0,gpt1
grubfm
```

Selecting an ISO and choosing “Boot with Ventoy” uses the exact same entry
point as the direct command. No `vt_*` calls are embedded in GRUBFM.

## Limits and tests

- The one-command boot path currently targets EFI Linux ISOs. BIOS, Windows
  ISO/WIM, VHD/VHDX, IMG, Unix and other boot methods have separate legacy
  paths but are **not** certified via this entry point yet.
- Physical file mappings are required for the Ventoy EFI consumer; merely
  opening an ISO through GRUB's Disk Pipeline does not guarantee compatibility.
- A fully booted guest OS is not implied by an EFI/Chain metadata smoke test.
- Existing module unload behavior is unchanged.
- The **unmodified legacy EFI consumer** can display an incorrect
  “not a standard Ventoy device” warning and pause for 10 seconds when the
  source disk has no Ventoy EFI partition. This has been reproduced with a
  QCOW2 disk whose Ventoy second-partition MBR entry is removed; the same
  command still reaches the Arch Linux EFI boot menu. A clean direct-boot
  experience on completely ordinary disks needs a versioned update to the
  EFI consumer, not a false `vtoy_reserved[6]` flag (that byte means VLNK
  and changes other Ventoy components' behavior).

On a host with the Ventoy test disk, Arch Linux ISO and OVMF:

```sh
tests/ventoy_boot_qemu.sh --check
tests/ventoy_boot_qemu.sh --boot
tests/ventoy_boot_qemu.sh --grubfm
tests/ventoy_boot_qemu.sh --plain
```

Only temporary EFI System Partition and OVMF variable images are written.
`VENTOY_TEST_DISK`, `VENTOY_TEST_ISO`, `VENTOY_BUILD`, `OVMF_CODE`,
and `OVMF_VARS` override the defaults. `VENTOY_QEMU_MEM` controls the
guest memory (3 GiB by default for current Arch ISO). The optional
`VENTOY_BOOT_CAPTURE=1 VENTOY_TEST_KEEP=1` captures QEMU screenshots for
manual diagnosis. A successful EFI menu check does not assert that the guest
reached a shell or login prompt.
