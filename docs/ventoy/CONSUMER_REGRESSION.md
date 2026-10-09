# WinPE EFI Consumer comparison: erroneous replay and corrected handoff

This document corrects the October 9, 2026 claim that the final Ventoy EFI
Consumer was broken. The final version was previously reported working. The
newly created QEMU test **did not reproduce its startup conditions**.
Do not use an incorrect test to justify changes to the Consumer binary.

## Reference from the working historical test

Preserved trace: `../out/testenv/work/qemu-serial.log.bak.new` (March 24).

- Ventoy runtime arguments: `isoefi= iso_udf debug`, with **no** forced
  `FirstTry` option for the Windows ISO.
- Windows OS chain: `vtoy_reserved=<0 0 1 1 0 0 0>`,
  `override_chunk_num=4`, `virt_chunk_num=1`,
  `virt_img_size_in_bytes=460988416`.
- Firmware `Connect partition driver Success`.
- `LoadImage` succeeded with a virtual
  `VenHw(...)/CDROM(0x1,0x144,0x1C770)/\\EFI\\BOOT\\BOOTX64.EFI` path.
- Subsequent EFI Block I/O operations appear in the trace. The archived
  serial stream by itself is **not a Windows desktop screenshot**.

Upstream Windows flow:
`../Ventoy/INSTALL/grub/grub.cfg` -> `uefi_iso_menu_func` ->
`uefi_windows_menu_func`. This selects `LoadIsoEfiDriver` according to
image compatibility/El Torito and does **not** unconditionally set `isoefi=on`
or `FirstTry` for Windows. See also
`grub-core/ventoy/ventoy_windows.c:2145-2162`.

## How the October test created a false regression

Our first `tests/ventoy_consumer_qemu.sh` had these **invented** options:

```text
set LoadIsoEfiDriver=on
chainloader ... isoefi=on iso_udf FirstTry=@EFI@BOOT@bootx64.efi ...
```

Additionally, GRUB 2.15's `fs_probe` selected `iso9660` for WIM
file access inside the dual-format WinPE ISO, while the reference
chain used `udf`:

| Condition | March reference | October incorrect test |
| --- | --- | --- |
| Consumer `isoefi` | empty | `on` |
| `FirstTry` | absent | forced |
| WIM metadata filesystem | UDF | ISO9660 |
| `vtoy_reserved[3]` | 1 | 0 |
| override chunks | 4 | 2 |
| virtual bytes | 460988416 | 460814336 |
| Partition driver | Success | Not Found |
| EFI image path | `VenHw/.../CDROM/.../BOOTX64.EFI` | `VenHw/.../bootx64.efi` |

This incompatible handoff led to three downstream `EFI_INVALID_PARAMETER`
responses, after which the Consumer returned to GRUB. Trying the stock
67,008-byte vs a 75,936-byte Consumer, or switching its extra UDF/ISO driver,
**did not make those incorrect test conditions equivalent to the final build**.
The conclusion that the final version was defective was unjustified.

## Correcting the test parameters

Removing the invented `isoefi=on` / `FirstTry` arguments immediately restored:

- `Connect partition driver Success`.
- `LoadImage` path with `/CDROM(0x1,0x144,0x1C770)/`.
- Windows EFI `StartImage` invocation without immediately returning
  `EFI_INVALID_PARAMETER`.

That solved only the first false regression. The next Windows Boot Manager
screenshot still reported `winload.efi` error `0xc0000001`, because its WIM
Chain was incomplete. Two additional prerequisites were missing:

- **UDF mapping and OS parameter:** For hybrid ISO/UDF media, GRUB module
  registration order made `fs_probe` open the WIM as ISO9660. The original
  Windows menu uses `vt_is_udf` to set `ventoy_fs_probe=udf`. The port now
  temporarily moves the already-registered UDF filesystem to the head of
  the GRUB filesystem list **only** while scanning Windows WIM patches, then
  restores the exact original list. It never registers a filesystem disabled
  by lockdown.
- **Ventoy runtime jump executable:** The new test omitted `vtoy_path` and
  `vtoyjump64.exe`. That 173,568-byte file rounds to exactly **174,080
  bytes** on 2,048-byte boundaries, matching the entire virtual-image
  shortfall. The runtime is now staged from the same version-pinned
  historical test disk and its SHA256 checked before QEMU starts.

## Completed WinPE GUI verification (2026-10-09)

With the unchanged, historical 75,936-byte EFI Consumer and unchanged WinPE
ISO, the repaired test generates these values, matching the March reference:

| Field | Working March reference | Current QEMU |
| --- | --- | --- |
| `isoefi` | empty | empty |
| `FirstTry` | absent | absent |
| WIM metadata FS | UDF | UDF |
| `vtoy_reserved` | `<0 0 1 1 0 0 0>` | identical |
| `override_chunk_num` | 4 | 4 |
| `virt_img_size_in_bytes` | 460988416 | 460988416 |
| GRUB Chain descriptor length | 1667736 | 1667736 |
| EFI file device path | `VenHw(...)/CDROM(...)` | same CDROM node |

**Actual user-space verification:** an isolated QEMU x86_64-efi boot advanced
past the Windows Boot Manager, loaded WinPE and displayed the full graphical
desktop, taskbar and application icons. The desktop was visually verified on
two separate single-VM runs: before and after strengthening jump-resource
error handling. The final automatic screenshot (2048x2048 PNG) and serial
logs were preserved at
`../out/testenv/work/winpe-verified-20261009/`. GUI success was assessed
from that screenshot, **not** from a non-returning `StartImage` or a timeout.

The native module now checks the existence and complete read of the jump
executable. Deliberately setting `vtoy_path` to a nonexistent directory
causes `/WEPE/WEPE64.WIM [FAIL]`, and `vt_windows_chain_data` explicitly
refuses to build the incomplete chain. A fault-injection QEMU run verified
both conditions.

## Windows-side vtoyjump ISO mount: verified

The WinPE **desktop alone is insufficient** to prove this product feature.
The October harness originally attached both disks through **VirtIO**, while
the March QEMU workflow used IDE/SATA. In a WinPE boot using VirtIO, the
original `vtoyjump64.exe` was executed, but its in-guest log at
`X:\Windows\system32\ventoy.log` showed it could only see drives D: and
X:, ending with `Failed to find ISO file`. The Windows desktop was visible
regardless. This was **another mistake in the test environment**, not a
reason to change the EFI Consumer.

The QEMU test now uses **IDE/SATA by default**. It attaches both the FAT EFI
test image and the original Ventoy test disk as **disposable writable QCOW2
overlays**, with untouched read-only RAW backing files. `VENTOY_GUEST_DISK_BUS=virtio`
retains the earlier disk topology for a negative control when useful.

The Windows user-space test was then repeated with unchanged pinned Consumer,
jump executable, and WinPE ISO. Opening `ventoy.log` from inside WinPE showed:

```text
File <C:\ISO\winpe.iso> exist
Disk UUID match
Disk check OK C: 1
OpenVirtualDisk success
VentoyAttachVirtualDisk success
Mount ISO FILE: SUCCESS
```

Windows Explorer also showed a DVD volume named `微PE工具箱` (UDF, 228 MiB),
matching the mounted original WinPE ISO. Its stable Explorer drive letter was
**E:**, although `vtoyjump` reported **F:** from an instantaneous drive-bit
difference. Do not assume the transient letter remains constant.

Screenshots directly from the running Windows guest, plus test inputs, are
retained in `../out/testenv/work/winpe-verified-20261009/`:

- `sata-vtoyjump-mount-log.png`: actual Windows `ventoy.log`.
- `sata-mounted-udf-dvd.png`: actual Explorer UDF DVD volume.
- `virtio-guest-ventoy-log.png`: negative control showing the original error.

**Result:** On the historical Windows-compatible disk bus, the previously
working WinPE EFI+WIM chain **also mounts the original ISO through vtoyjump**.
This was verified in the guest, not inferred from successful firmware launch.
The guest mount remains a **manual visual assertion**, not an automatically
machine-parsed pass condition in `--boot`.

**Separate observation:** two QEMU guests run concurrently once displayed
a Windows interactive-logon initialization error. The subsequent solo
test did not reproduce it. The cause has not been established.

**Scope:** Windows boot here uses the current `ventoy.mod` **internal
`vt_*` command sequence**. The public `ventoy PATH` single-command entry
is still restricted to Linux ISOs, and the GRUBFM GUI Windows action has
not been implemented.

## Test modes and legitimate assertions

```sh
tests/ventoy_consumer_qemu.sh --prepare
# Assert UDF, complete WIM patch and golden Chain length (1667736).

tests/ventoy_consumer_qemu.sh --trace
# Assert golden OS parameter, four Overrides, virtual bytes,
# CDROM DevicePath and successful Windows EFI StartImage entry.

VENTOY_TEST_KEEP=1 VENTOY_SCREENSHOT=1 VENTOY_CAPTURE_AFTER=135 \
  tests/ventoy_consumer_qemu.sh --boot
# Saves an actual guest framebuffer; inspect winpe-screen.png visually.
# The script intentionally returns 2 UNVERIFIED on QEMU timeout.

tests/ventoy_consumer_qemu.sh --invalid-params
# Deliberately wrong isoefi=on + forced FirstTry NEGATIVE CONTROL only.
# NEVER cite its failure as evidence against the working Consumer.
```

The test pins the historical Consumer, ISO/UDF drivers and the
`vtoyjump64.exe` SHA256
(`5787798ecc49c6a179bca0b6b07854b9f46a3f96b11731f8d1c4cc6fe8b5ecce`),
uses disposable EFI System Partition/OVMF variables and, by default,
IDE/SATA guests with writable QCOW2 overlays backed by unchanged RAW files. `VENTOY_RUN_ROOT=../out/testenv/work` and
`VENTOY_TEST_KEEP=1` preserve diagnostics.

The newly added `extras/ventoy/consumer-status.patch` was based on the
invalid comparison and **has been withdrawn** without being compiled,
applied or installed. The underlying source's unconditional
`EFI_SUCCESS` on exit is an independent code fact, but not evidence that
the confirmed working boot flow fails.

## Follow-up

- The verified WinPE ISO+EFI Consumer now reaches GUI in QEMU using the
  internal `vt_*` path. Implement Windows support in the high-level
  `ventoy PATH` command and GRUBFM only after it reuses this exact plan,
  UDF override and jump runtime; do not fork another parameter generator.
- Formalize the now-verified in-guest `vtoyjump` log/volume checks as
  machine-readable QEMU assertions if needed. The saved screenshots prove
  this exact test, but a QEMU timeout alone remains UNVERIFIED.
- Investigate the one-off concurrent-QEMU interactive-login error separately.

Do not change or rebuild EFI Consumer based on the obsolete forced-ISOEFI
failure; the legacy Ventoy EDK2 source tree also contains preexisting deleted
build files that were deliberately left untouched.
