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

This isolates the **first** false regression. A screenshot of this test still
showed Windows Boot Manager reporting `winload.efi`, status `0xc0000001`.
The newer script's WIM override data still differs from the historical
working UDF chain; the screenshot therefore does NOT establish a regression
in the final Consumer. The exact cause of this **remaining test mismatch**
requires its own faithful UDF chain comparison.

## Test modes and legitimate assertions

```sh
tests/ventoy_consumer_qemu.sh --prepare
# Current GRUB WIM chain producer only; NOT final-version equivalence.

tests/ventoy_consumer_qemu.sh --trace
# Uses original Windows isoefi/FirstTry semantics, records EFI DevicePath,
# and checks passage to Windows StartImage. NOT desktop verification.

tests/ventoy_consumer_qemu.sh --boot
# Attempts the same handoff without diagnostic keyboard events;
# QEMU timeout/nonreturn by itself is NOT a PASS.

tests/ventoy_consumer_qemu.sh --invalid-params
# Deliberately wrong isoefi=on + forced FirstTry NEGATIVE CONTROL only.
# NEVER cite its failure as evidence against the working Consumer.
```

The test pins the historical Consumer and ISO/UDF driver SHA256, uses a
disposable EFI system disk/OVMF variables and attaches source RAW disks
read-only. `VENTOY_RUN_ROOT=../out/testenv/work` and
`VENTOY_TEST_KEEP=1` preserve diagnostics.

The newly added `extras/ventoy/consumer-status.patch` was based on the
invalid comparison and **has been withdrawn** without being compiled,
applied or installed. The underlying source's unconditional
`EFI_SUCCESS` on exit is an independent code fact, but not evidence that
the confirmed working boot flow fails.

## Follow-up test work

1. Recreate the historical WinPE UDF WIM override map, compare the 4
   override chunks and virtual length byte-for-byte before launching.
2. Reproduce the final version's complete EFI and QEMU disk configuration,
   including boot-media topology and boot catalog.
3. Verify reaching actual WinPE user space rather than merely loading its
   EFI boot manager. Treat EFI protocol and OS-stage results separately.

Do not change or rebuild EFI Consumer based on the obsolete forced-ISOEFI
failure; the legacy Ventoy EDK2 source tree also contains preexisting deleted
build files that were deliberately left untouched.
