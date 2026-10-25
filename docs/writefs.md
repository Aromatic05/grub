# WriteFS: explicit writable filesystem API

WriteFS is a separate **explicitly mounted** writable filesystem subsystem.
GRUB's original read-only VFS retains automatic filesystem probing and its
existing `grub_file_open()` API. It never gains implicit write access.

## Discovery and mounting

```grub
mount filesystems
mount --list-filesystems
mount (hd0,gpt2) 1
mount status
umount 1
```

The build produces two platform manifests:

- `writefs.lst`: names of loadable writable drivers; read on mount.
- `writefs-types.lst`: filesystem types and their driver modules; read on
  query, with **no write or module-loading side effect**.

Writable backends currently support FAT12, FAT16, FAT32 and exFAT through
`fatfs.mod`, and NTFS through `ntfs3g.mod`. Listing support does not mean
a particular volume is safe to mount: mount-time disk checks remain enforced.

A volume must be explicitly mounted into slots 1 to 9. During GRUB
Secure Boot Lockdown, new writable mounts and all mutating WriteFS
operations (including Lua calls) are denied by Core; ordinary read-only VFS
operations remain available. An existing mount may be unmounted to clean up.

File operations accept only paths like `1:/example.txt`; normal GRUB paths such as
`(hd0,gpt2)/example.txt` remain read-only. CLI file-copy commands may
explicitly copy from read-only GRUB paths to already-mounted WriteFS paths.

## Native WriteFS API

The public declarations are in `include/grub/writefs.h`.

```c
grub_writefs_mount("(hd0,gpt2)", 1);
struct grub_writefs_file file;
grub_writefs_open(&file, "1:/example.txt", GRUB_WRITEFS_READ);
grub_writefs_read(&file, buffer, length);
grub_writefs_close(&file);
grub_writefs_umount(1);
```

Public operations include `open`, `read`, `write`, `seek`, `truncate`,
`close`, `mkdir`, `unlink`, `rename`, `touch`, explicit `mount`/`umount`,
mounted-volume metadata (`get_device`, `get_label`, `set_label`), and
`iterate_filesystems`. For new files use the
`GRUB_WRITEFS_WRITE | GRUB_WRITEFS_CREATE` flags. The caller provides
`struct grub_writefs_file` storage; after a successful open it must call
`grub_writefs_close` exactly once, even if subsequent operations fail.

The core owns mount validation, open-file accounting and driver dispatch. A
writable volume with open files cannot be unmounted. The original GRUB VFS
does not share the WriteFS handle namespace.

`grub_writefs_iterate_filesystems` takes a callback receiving a filesystem
type string, backend module name and whether the module is already loaded.
It merges installed types with dynamically registered drivers, deduplicates
filesystem names, and reports manifest I/O errors. Enumeration does not
load a driver or mount a volume.

`grub_writefs_get_device` returns an allocated device identity such as
`(hd0,gpt2)`. `grub_writefs_get_label` returns an allocated UTF-8 label.
The caller frees these output strings. `grub_writefs_set_label` mutates the
mounted volume and is denied during Lockdown. Both FAT/exFAT and NTFS
implement the label operations.

## Lua API

The FatFs-specific `fat.*` table has been retired. WriteFS bindings are
available as `writefs.*` whenever the `lua` module is loaded:

```lua
local types = writefs.supported()
for _, fs in ipairs(types) do
  print(fs.name, fs.driver, fs.loaded)
end

writefs.mount("(hd0,gpt2)", 1)
local file = writefs.open("1:/example.txt", 6)  -- WRITE | CREATE
writefs.write(file, "hello")
writefs.seek(file, 0)
writefs.close(file)
writefs.umount(1)
```

The following functions are exposed: `supported`, `mount`, `umount`,
`disk_status`, `get_label`, `set_label`, `open`, `close`, `read`, `write`,
`seek`, `tell`, `size`, `eof`, `truncate`, `mkdir`, `unlink`, `rename`,
`touch`. `disk_status(number)` returns a device identity string or nil
for an unmounted slot, instead of the retired `fat.disk_status` raw pointer.

This Lua build uses 32-bit numbers. To seek past 2 GiB, pass an exact
unsigned decimal string: `writefs.seek(file, "2147483648")`. `tell` and
`size` return a number within 32-bit signed range or an exact decimal
string beyond it. `write` returns a byte count only on complete success;
incomplete writes raise a Lua error, and close may independently report
a delayed filesystem flush error.

`writefs.open` takes a `N:/path` and a numeric flag mask: READ=1,
WRITE=2, CREATE=4. Its return value is typed Lua userdata, not a raw
FatFs pointer. An explicit close invalidates the handle; garbage collection
also closes handles that have not been explicitly closed. Opening without a
mounted writable volume fails.

This Lua binding is a focused API migration; general-purpose language
binding infrastructure and lazy hooks are intentionally out of scope.

## Testing

`tests/writefs_qemu.sh` exercises FAT, exFAT and NTFS operations in QEMU,
checks that open files prevent unmount, and verifies on-disk persistence
using independent host-side tools. Use disposable volumes when testing writes.
