# Disk Pipeline

## Scope

This is the construction pipeline for **file-backed virtual disks**. It does not
replace GRUB's disk drivers, filesystem drivers, file API, or normal disk cache.
Construction takes place once; the read/write hot path uses the resulting file
view and optional sparse COW overlay without re-running stage hooks.

The implementation intentionally keeps `grub_file_t` as the view/reader
interface. It does not create a parallel class hierarchy of files, readers,
parsers, providers, and backends.

## Ordered stages

| Stage | Contract | Current implementation |
| --- | --- | --- |
| SOURCE | Open backing bytes without implicit decompression | Shared file source, optional blocklist or RAM copy |
| VIEW | Compose zero or more ordered file transformations | Gzip, XZ, LZOP and Zstd registered on the shared Stage hook list |
| FORMAT | Select exactly one compatible disk-container parser when requested | QCOW2, VHDX, VMDK, fixed VDI, VHD via `vhd.mod` |
| BLOCK | Add block-level semantics without modifying the underlying format | Base Reader/Writer plus optional 4 KiB sparse COW hook |
| PUBLISH | Register one device endpoint | GRUB Loopback or EFI Map Block I/O |

The stages execute in the fixed SOURCE -> VIEW -> FORMAT -> BLOCK -> PUBLISH
order. Within a stage, hooks are sorted by the `order` field. SOURCE, FORMAT
and PUBLISH select one matching hook. VIEW and BLOCK may execute several
transform hooks. Each hook has an optional probe and a run callback.

FileView and virtual-disk VIEW now share the Stage hook registry. The VIEW
dispatcher restarts probing after each successful transformation, up to eight
nested layers. An XZ stream inside Gzip and a Gzip stream inside XZ use the
same ordered composition without an additional registry.

The VIEW stage runs transparently by default, preserving the earlier
GRUB Alive/grubfm behavior. The `-D` flag is accepted for compatibility and
does not change the default. The FORMAT stage is **explicit**:
`vhd NAME FILE` and `map -F FILE [NAME]` request format detection, while
plain `loopback NAME FILE` and `map FILE [NAME]` treat the decompressed bytes
as a RAW disk.
A matching but malformed container must report a parser error, not silently
be treated as RAW.

The format parser registry is no longer indexed by
`GRUB_FILE_FILTER_*`; no format parser API is held by `loopback.mod`.
`vhd.mod` registers FORMAT hooks with the built-in pipeline.

## I/O and ownership

SOURCE produces a file owned by the pipeline. VIEW may wrap it with a
decompressed view. A successful FORMAT parser owns its backing view; a failed
FORMAT parser leaves the input view owned by the pipeline. During a VIEW
failure, FileView closes its consumed view chain and clears the pipeline's
file pointer. This avoids a second close when the pipeline fails.

Closing a device first unpublishes it, then unwinds the BLOCK reader chain,
closes the file/view/parser chain, releases any owned `-m` RAM buffer, and
releases FORMAT module references. A successful PUBLISH pins its owning module
until the device is deleted; a successful FORMAT pins the parser module until
its file is closed. EFI Map uses EFI page allocation for `-m` and preserves
`--rt` reserved-memory semantics. Externally supplied `mem:` pointers are
**not** freed by the pipeline. If an EFI protocol refuses to uninstall, its
backing resources remain alive and an explicit `map -x` can retry.

Because the kernel reserves the GRUB disk name `mem`, PUBLISH checks for
collisions with *all* existing disk devices. An inaccessible alias must not
silently hide a firmware disk or GRUB's built-in memory disk.

All logical I/O uses byte offsets. The format read callback does not advance
`grub_file_t.offset` itself; `grub_file_read()` advances that offset exactly
once. All short reads and failed seeks return errors, except a read over the
logical end of the image, which zero-fills the unbacked suffix.

### Write behavior

| Input | Default write behavior | `-C` |
| --- | --- | --- |
| RAW, disk-backed | Existing direct-write path when supported by the filesystem | Volatile COW |
| RAW, copied to RAM (`-m`) | Modify the owned RAM copy | Volatile COW |
| XZ/Gzip/nested compression | Volatile COW | Volatile COW |
| VHD/VHDX/QCOW2/VMDK/VDI parser output | Volatile COW | Volatile COW |

The COW hook stores modified 4 KiB blocks in memory (currently capped at
256 MiB). It reads known modified blocks directly, without a redundant
backing-file read or decompression, and delegates missing blocks to the lower
reader. It does not rewrite or recompress the source file. **Writes are lost
when the mapping is deleted.** Persisting changes to an `.xz` file requires
a separate materialize/recompress/atomic-commit feature; it is not implemented.

## Module boundaries

`grub-core/io/disk_pipeline.c` is a built-in kernel facility. The public
interface lives in `include/grub/disk_pipeline.h` and is included in GRUB's
export-symbol header list. Host utilities also link it via
`Makefile.util.def`.

SOURCE and COW BLOCK hooks are built into the common disk layer. FileView
codec modules register VIEW hooks, while `vhd.mod` registers FORMAT hooks.
`loopback.mod` and `map.mod` implement only their respective PUBLISH
protocols. Neither must depend on `vhd.mod`; the shared CRC32C used by VHDX
is part of the kernel rather than depending on `btrfs.mod`.

EFI Map registers its Block I/O handles through PUBLISH and uses the same
byte-offset BLOCK Reader/Writer as GRUB Loopback. EFI protocol uninstallation
must precede destruction of the underlying buffers. El Torito aliases share
the parent backing pipeline. Ventoy physical file extent collection remains
separate from this virtual-disk construction pipeline and still needs its own
cleanup.

## Verification

After building the x86_64 EFI target, run from the repository root:

```sh
tests/disk_pipeline_qemu.sh "$PWD/build-x86_64-efi"
```

The regression runner uses QEMU + OVMF, creates new file-backed FAT images in
a temporary directory, and attaches both data and EFI system images read-only.
It checks RAW, XZ, Gzip, both nested compression orders, dynamic VHD inside
XZ, blocklist, RAM copy, COW writes, reopen rollback, parser rejection,
EFI Map publication (`map -n`), EFI VHD.XZ parsing (`map -F`), EFI COW
(`map -C`), successful `map -x` unpublishing, duplicate disk names and
collisions with built-in/physical disk names.

Dependencies: `qemu-system-x86_64`, `qemu-img`, `mkfs.vfat`, Mtools,
`xz`, `gzip`, OVMF, `timeout`, and `sha256sum`. Set `OVMF_CODE` and
`OVMF_VARS` if the firmware is not installed under
`/usr/share/edk2/x64/`. The test leaves logs only on failure.

The initial implementation was built for x86_64 EFI and i386 PC and exercised
in QEMU on x86_64 EFI. On the GCC 16 build host, unrelated legacy GRUB/Ventoy
warnings require `CFLAGS='-O2 -g -Wno-error'` for test builds. This is not
a source-level waiver for warnings introduced by the pipeline.

## Remaining work

- Add EFI Block I/O protocol failure injection and partial-install rollback
  tests; QEMU currently validates normal `map -x` behavior.
- Keep file extent discovery separate from block-read hooks.
- Extend malformed-container, partial-failure and repeated-lifecycle tests
  to the other image parsers (VHD parser failure and recovery are covered).
- Benchmark compression seek costs and add an optional cache at BLOCK if
  data justifies it.
- Add a separate explicit, durable writeback/commit design if required.
