# EFI Consumer / WinPE regression (2026-10-09)

This is a source-and-QEMU fact record, not a claim that Windows boots.
The preserved March 2026 test fixtures live in the sibling `out/testenv`
repository. Its pre-unification commands (`ventoycore`, `ventoywindows`,
`vtwindows`, `vtchainloadwin`) are NOT valid entry points for the current
`ventoy.mod`.

## Reproduction

From the GRUB repository root:

```sh
# Chain producer only; checks physical extents, BCD WIM discovery and chain.
tests/ventoy_consumer_qemu.sh --prepare

# Expected-BUG test: returns 0 only while the historical *masked success*
# failure is present. Once Consumer is fixed, this test must be retired.
tests/ventoy_consumer_qemu.sh --repro

# Boot regression: fails when Consumer returns or boot success is unverified.
tests/ventoy_consumer_qemu.sh --boot

# Consumer protocol debug trace: QEMU monitor advances its debug pauses.
# A failure is EXPECTED with the historical binary; logs are kept.
tests/ventoy_consumer_qemu.sh --trace
```

Set `VENTOY_TEST_KEEP=1` to retain passing tests; failing runs are always
retained. `VENTOY_RUN_ROOT=../out/testenv/work` gives persistent local records.
The test mounts both EFI and ISO source disks READ-ONLY. It creates its own
writable OVMF variables in a fresh directory. Its Consumer and ISO/UDF
filesystem drivers are verified against pinned SHA256 hashes *before* QEMU
starts. Testing a different set requires deliberate runtime and hash overrides.

Pinned historical runtime (`../out/testenv/work/ventoy-runtime-p1`):

| Artifact | SHA256 |
| --- | --- |
| `ventoy_x64.efi` (75,936 bytes) | `362612542b480fb1417e6c7fc498bc42214b8266d237e319f116c0f7a3f00538` |
| `udf_x64.efi` | `64b39735f98129451373ff36b281cd93f678f96dc173eb4dea78104a161622c4` |
| `iso9660_x64.efi` | `e3412168c2750fc81ca3a546261cf7cbfbf8b7b0212ccd6e8a7b26a40c1f8e22` |

This 75,936-byte test binary is **not** interchangeable with the 67,008-byte
`../Ventoy/INSTALL/ventoy/ventoy_x64.efi` or the 68,992-byte compiled image
found in `../Ventoy/EDK2/.../Build/MdeModule/.../X64/Ventoy.efi`.

## Evidence and exact observed boundary

On the existing `out/testenv/images/ventoy-test.raw` with its real WinPE ISO,
current `ventoy.mod` reaches:

```
WINPE_EXTENT_OK
WINPE_LOOPBACK_OK
WINPE_WIM_PATCH_COUNT=1
WINPE_CHAIN_READY
WINPE_CONSUMER_BEGIN
```

The 2026-10-09 non-debug run reached `WINPE_CONSUMER_RETURNED`. The GRUB EFI
chainloader reported `StartImage status=0x0`, even though WinPE never booted.
`--repro` asserts exactly that erroneous outcome. `--boot` rejects it.

The QEMU debug trace (`../out/testenv/work/ventoy-winpe.NwJiZR/serial.log`)
records three separate attempts by the Consumer to boot
`\\EFI\\BOOT\\bootx64.efi`; each downstream `StartImage` returns
**`EFI_INVALID_PARAMETER`**. The Consumer then exits and GRUB still sees
`StartImage status=0x0` (success). This is the same key failure class as the
archived March log
`../out/testenv/work/qemu-serial.log.bak_20260323_234607`.

The trace reports successful installation of the virtual Block I/O protocol,
successful connection of the Disk I/O driver, and loading the UDF driver.
The `Partition Driver` connection reports `Not Found`, but this is **not
proven** to be the cause of Windows' `EFI_INVALID_PARAMETER`.
The boot image's `LoadedImage` device path is only `VenHw(...)` and lacks an
explicit `CDROM(...)` segment. This is a concrete difference to examine
against EFI Map's El Torito device path, **not yet a verified root cause**.

The debug run does **not** show `ventoy_wrapper_file_open` invocations;
therefore, the previous hypothesis that repeated filesystem hooks caused
*this specific* WinPE failure is unsupported. Hook rollback remains an
independent defect to test with explicit file replacement enabled.

## Source defects for a later Consumer patch

All locations below refer to the existing
`../Ventoy/EDK2/edk2_mod/edk2-edk2-stable201911/MdeModulePkg/Application/Ventoy/`
source (not modified by this test work):

1. `Ventoy.c`: `VentoyEfiMain()` returns `EFI_SUCCESS` at the end regardless of
   the recorded `ventoy_boot()` failure; `ventoy_boot()` also defaults to
   success if it found a bootable file, even when `StartImage` failed.
2. `Ventoy.c`: `ventoy_boot()` overwrites the
   `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL.OpenVolume` function pointer but does not
   restore it when `StartImage` returns. Test repeated attempts in a single
   EFI session *with file replacement enabled* before changing this path.
3. `VentoyProtocol.c`: `ventoy_wrapper_file_read()` assumes a 2048-byte-aligned
   file offset, disregards the underlying Block I/O status, and reports
   success for short/non-sector-sized reads. `ventoy_wrapper_file_get_info()`
   checks only an exactly-zero buffer length, not every short buffer.
4. `VentoyProtocol.c`: virtual-sector remap reads do not propagate all
   `ventoy_read_iso_sector()` failures.

The historical `Ventoy` repository has pre-existing deleted EDK2 build
bootstrap/DSC files and other unrelated modifications. They were deliberately
NOT restored/reset. No new Consumer binary was built from this incomplete
source state and no unvalidated patch has been installed into the runtime.

## Next verification gates

- Rebuild the Consumer from a **complete, reproducible EDK2 toolchain** in an
  isolated build environment, without restoring the user's deleted files.
- Change and test status propagation first; `EFI_INVALID_PARAMETER` from the
  downstream Windows image must not become `EFI_SUCCESS` at the GRUB boundary.
- Test `EFI_FILE_PROTOCOL.GetInfo` short buffers, unaligned `Read`, actual
  read failures and repeated OpenVolume hooks with a dedicated EFI test app.
- A/B comparison: legacy `VenHw(...)` path vs EFI Map `VenHw(...)/CDROM(...)`,
  while holding the WinPE ISO and EFI binary version fixed.
- Treat Windows desktop/PE shell arrival as distinct from Chain construction,
  Block I/O install, `LoadImage`, or a non-returning `StartImage`.
