# GRUB

**An extended GNU GRUB fork for advanced boot workflows, filesystem access, and scripting.**

This project builds on [GNU GRUB](https://gitlab.freedesktop.org/gnu-grub/grub)
with additional boot tools, virtual-disk support, filesystem drivers, and an
interactive file manager. It brings together features originating from a1ive's
GRUB projects and Ventoy with new development on the modern GRUB codebase.

The source tree currently targets **GNU GRUB 2.17 development**. This is an
independent fork, not an official GNU GRUB or Ventoy release.

## Features

### Boot and disk management

- **Direct ISO boot:** The integrated `ventoy` command can boot supported Linux
  and Windows/WinPE ISOs without requiring GRUBFM or a dedicated menu.
- **Windows boot tools:** `ntboot`, `wimboot`, disk mapping, and related
  Windows boot helpers.
- **Virtual disks:** Modules for selected VHD, VHDX, VDI, QCOW2, and VMDK image
  layouts. Supported operations and image variants differ by format.
- **Firmware and boot utilities:** Extended BIOS/UEFI disk, firmware,
  environment, and compatibility commands.

### Filesystem support

- **Additional readers:** APFS, ReFS, FBFS, LynxFS, QNX4, and QNX6, alongside
  GNU GRUB's existing filesystem modules.
- **WriteFS:** A shared writable-filesystem subsystem built into GRUB Core.
  It provides mount management and common file operations including
  `mount`, `umount`, `cp`, `mv`, `mkdir`, `rm`, `rename`, `touch`, and
  `write_file`.
- **Modular write drivers:** FAT/exFAT via `fatfs.mod` and NTFS via
  `ntfs3g.mod`. Drivers register their own probe and file-operation
  callbacks; the build generates `writefs.lst` for automatic discovery and
  `writefs-types.lst` for filesystem-type enumeration without loading drivers.
  Additional write drivers can use the same interface.

The additional filesystem readers are **not** automatically writable.
WriteFS currently has FAT/exFAT and NTFS write backends.

### File management and scripting

- **GRUB File Manager (`grubfm`):** Device and file browsing with configurable
  file handling and text/hex viewing.
- **Lua:** An embedded scripting runtime with GRUB-specific APIs.
- **INI and command extensions:** Configuration parsing and additional
  commands for interactive use and boot automation.

## Getting started

See [INSTALL](INSTALL) for GNU GRUB build and installation instructions.
The repository also includes [build-all-targets.sh](build-all-targets.sh)
for the configured x86/ARM targets; platform validation differs by target.

### Direct ISO boot

On supported x86_64 UEFI setups, load the Ventoy module and point it at an
ISO on a physical disk:

```grub
insmod ventoy
ventoy --check (hd0,gpt2)/ISO/winpe.iso
ventoy --runtime=(hd0,gpt1)/ventoy (hd0,gpt2)/ISO/winpe.iso
```

The runtime directory and ISO can reside on different partitions. You need
the matching original Ventoy runtime files; `--check` validates prerequisites
but does not boot the guest OS. See the
[direct-boot guide](docs/ventoy/DIRECT_BOOT.md) for details.

### Filesystem writes

WriteFS commands are provided by GRUB Core; there is no separate
`writefs.mod`. The appropriate driver is discovered **only when explicitly
mounting** a writable volume. `mount filesystems` lists installed writable filesystem
types without mounting or loading the corresponding modules; `mount status`
shows active mounts. WriteFS uses a separate `N:/path` namespace and does
not grant write access through GRUB's read-only `grub_file_open()`.

```grub
mount filesystems
mount (hd0,gpt2) 1
cp (hd0,gpt1)/boot.cfg 1:/boot.cfg
umount 1
```

Lua exposes the filesystem-independent `writefs.*` API (including
`writefs.supported()`, `open`, `read`, `write`, `close`, and directory
operations). The former FatFs-specific `fat.*` Lua API has been retired;
native FatFs functions remain internal to its driver. See
[WriteFS API documentation](docs/writefs.md).
Use disposable test volumes when trying write operations.

## Testing and compatibility

- **x86_64 UEFI and BIOS:** The WriteFS QEMU suite passes 131 assertions on
  each platform, including FAT/exFAT/NTFS persistence and independent
  host-side checks. See [tests/writefs_qemu.sh](tests/writefs_qemu.sh).
- **ISO boot:** Arch Linux and Windows/WinPE direct-boot paths have been
  exercised on x86_64 UEFI. Other operating systems, firmware modes, and
  combinations have not all been validated.
- **Other architectures:** Build targets exist, but ARM64 WriteFS integration
  has not completed full build and boot validation.
- **Known limits:** Some Ventoy boot modes (including Unix ISO and
  FydeOS/ChromeOS IMG boot) are unavailable. Advanced virtual-disk features
  such as QCOW2 backing chains are not universally supported.

This is actively developed boot software. Test individual features on
noncritical media before relying on them for important systems.

## Documentation

- [Ventoy direct boot](docs/ventoy/DIRECT_BOOT.md)
- [Ventoy quick start (Chinese)](docs/ventoy/quickstart_zh.md)
- [a1ive GRUB reference documentation (Chinese)](docs/alive/README.md)
- [WriteFS QEMU regression suite](tests/writefs_qemu.sh)
- [ntfs-3g upstream findings](tests/ntfs3g/README.md)
- [Migration history and compatibility notes](docs/porting/MIGRATION_PROGRESS.md)

## Origins and acknowledgments

This fork is based on [GNU GRUB](https://gitlab.freedesktop.org/gnu-grub/grub)
and incorporates work from [a1ive](https://github.com/a1ive) and
[Ventoy](https://github.com/ventoy/Ventoy), alongside contributions from
other upstream projects and developers.

Special thanks to **a1ive** for contributing directly to this repository.
[Pull request #1](https://github.com/Aromatic05/grub/pull/1) added APFS and
ReFS support and reworked the FBFS driver.

## Contributing

Bug reports, patches, tests, and compatibility improvements are welcome.
See [CONTRIBUTING](CONTRIBUTING).
