# ntfs-3g upstream findings

Upstream: https://github.com/pbatard/ntfs-3g

Baseline: `68c1da7ec35cfa3289bfbe3999fb31474494abce` (unmodified upstream sources).
Both cases were reproduced with AddressSanitizer; the GRUB backend is not needed to reproduce them.

| Case | Reproducer | Expected | Observed |
| --- | --- | --- | --- |
| Failed unmount releases volume | `upstream_umount_lifetime.c` | When `ntfs_umount()` returns an I/O error, the volume remains valid as documented. | A deliberately failing `sync` makes `ntfs_umount()` return `-1`, but the volume has already been freed. Accessing it afterwards yields `heap-use-after-free`. |
| Invalid `$UpCase` cleanup | `upstream_mount_invalid_upcase.c` | An invalid `$UpCase` attribute causes a clean mount failure. | A one-byte `$UpCase` attribute leads to `heap-use-after-free` during `ntfs_attr_close()` in the failed-mount cleanup path (`libntfs-3g/attrib.c:600`). |

## Reproduction

Build the upstream library with `-O1 -g -fsanitize=address -fno-omit-frame-pointer` and `-fsanitize=address` in the linker flags. Compile each reproducer against that library with `-fsanitize=address`, then run against a fresh 64 MiB NTFS image created with `mkntfs -F -Q`.

For the invalid `$UpCase` test, first create a one-byte file and write its contents to MFT record 10 using `ntfscp -f -i <image> <one-byte-file> 10`. The failed-unmount test supplies its own failing device `sync` callback. Run each reproducer with the image path as its sole argument.

These are upstream defect reports, not changes required in the vendored upstream source. The GRUB adapter accounts for the current unmount lifetime behavior, and the QEMU suite includes a repeated invalid-volume mount regression.
