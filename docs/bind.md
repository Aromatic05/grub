# GRUB native bindings

GRUB Core provides a language-independent native binding registry. Native
modules publish typed operations; a runtime such as Lua maps its own values
onto those operations. Registration **does not** load Lua or another scripting
runtime.

This ABI is internal to a build of GRUB. It does not promise binary
compatibility across GRUB versions, and it is not a general-purpose
`dlsym`, C calling-convention interpreter, or raw memory FFI.

## Source layout

- `include/grub/bind.h`: types, registration and invocation contract
- `grub-core/kern/bind.c`: registry, dispatch and handle lifecycle
- `grub-core/script/lua/grub_bind.c`: Lua-specific mapping and lazy lookup
- Original native modules: export typed functions without including Lua

The registry maps `namespace.method` to a unique, never-reused invocation
ID. Each function publishes its exact required and optional parameter types.
`grub_bind_invoke` validates types, references the owning GRUB module
during the call, and releases it before returning.

The public value types are nil, boolean, signed/unsigned 64-bit integer,
string, bytes (both with explicit lengths), small named typed records and
managed resource handles.
Results use an array with at most `GRUB_BIND_MAX_RESULTS` elements, so
functions can return multiple values without a Lua-specific calling
convention.

## Native modules

For example, an independently loadable native module can expose a function:

```c
static const enum grub_bind_type args[] = { GRUB_BIND_STRING };

static grub_err_t
get_value (grub_size_t argc, const struct grub_bind_value *argv,
           struct grub_bind_value *out, grub_size_t *count)
{
  const char *value = native_lookup (argv[0].value.bytes.data);
  out[0].type = value ? GRUB_BIND_STRING : GRUB_BIND_NIL;
  if (value)
    {
      out[0].value.bytes.data = value;
      out[0].value.bytes.size = grub_strlen (value);
    }
  *count = 1;
  return GRUB_ERR_NONE;
}

static const struct grub_bind_export exports[] = {
  { "settings", "get", args, 1, 1, get_value }
};

GRUB_MOD_INIT(settings)
{
  grub_bind_register (mod, exports, 1);
}

GRUB_MOD_FINI(settings)
{
  grub_bind_unregister (settings_module);
}
```

The descriptor and callback live in the native module. The module must
unregister its exports during finalization. Native implementations return
GRUB error codes; they must not call a language runtime or longjmp through
`grub_bind_invoke`.

The descriptor and callback example above is illustrative. In a complete
module, retain `mod` in a module-local variable for use in the FINI function.

## Registration, lazy lookup and unloading

1. Native module loads: registers descriptors only, without loading Lua.
2. Lua loads: attaches a runtime observer and exposes namespace proxies.
3. A script accesses `ini.get`: the Lua adapter looks up the current
   export ID and creates a closure pointing at its own stable dispatcher.
4. Module unloads: exports are revoked. A closure saved before unloading
   no longer resolves to executable native code.
5. Module reloads: newly accessed functions have new IDs; old closures stay
   revoked.

The observer is notification-only. It never executes the Lua VM from a
native module initializer/finalizer. Lua synchronizes on command entry and
supports lazy global namespace lookup for modules loaded within a session.

## Objects and lifetime

Use `grub_bind_handle_create(owner, type, data, destroy)` for native objects.
Creating a handle references the native owner module, preventing its code from
being unloaded while the object is live. `grub_bind_handle_close` releases
its native resource and module reference but leaves the opaque handle
allocation intact. `grub_bind_handle_release` frees the allocation and
closes an open resource if necessary.

The Lua adapter wraps handles in typed userdata and registers a GC finalizer.
Scripts can save aliases to a handle, but methods reject a closed object.
Consumers in other languages must call release on their own resource wrapper.

Input string/bytes are borrowed for one invocation. Returned string/bytes
may also be borrowed or may transfer a heap allocation using `owned = 1`.
Consumers call `grub_bind_value_dispose` after converting each result. Native
handle results transfer ownership to the runtime.

### Named records

`GRUB_BIND_RECORD` describes a small, language-neutral collection of typed
named fields. The Lua adapter maps an ordinary table with scalar fields to a
borrowed record during invocation. For example, the existing video color
`{r=255, g=128, b=0, a=255}` remains a Lua table; the video module receives
typed values, never a `lua_State`. Omitted alpha defaults to 255. The
adapter limits this small record to eight fields, with no nested structures.
Returned record fields may be owned by the exporting module, in which case
the result disposer releases the fields and their values.

## Lua numeric conversion

This fork currently uses an integer-based Lua 5.1 numeric representation
(`LUA_NUMBER = int`). Native bindings retain exact signed/unsigned 64-bit
integers. Values outside the Lua 32-bit signed numeric range are returned
as decimal strings; decimal strings are accepted for 64-bit native arguments.
This is deliberate: large disk offsets must never be silently truncated.

## Migrated modules

| Module | Lua namespace | Notes |
| --- | --- | --- |
| `conv` | `gbk.*` | string conversions |
| `ini` | `ini.*` | managed INI handles |
| `getkey` | `input.*` | optional arguments, two-value key results |
| `wimboot` | `wim.*` | WIM queries using the read-only GRUB VFS |
| `dd` | `disk.*` | owned disk handles, binary-safe reads and explicit raw writes |
| `memrw` | `memrw.*` | original raw memory read/write functions, U64 addresses |
| `videoinfo` | `video.*` | typed color records, managed bitmap handles |
| EFI `dp` | `efi.*` | copied EFI Device Paths, managed `efi.dp` handles |

Each module registers only its existing native operations and no longer
references `lua.mod`. The original GRUB commands keep their existing names.

### Compatibility and capability boundaries

- `disk.open()` returns managed Lua userdata instead of `lightuserdata`.
  `disk.close()` explicitly invalidates it. Likewise, bitmap loading and
  scaling return managed `video.bitmap` handles and `efi.getdp()` returns a
  copied `efi.dp` handle. Old raw pointer values cannot be passed into the
  new API. These intentional changes prevent use-after-free.
- `efi.getdp()` accepts the managed handle from `disk.open()`, and
  `efi.dptostr()` accepts only its copied device-path handle. EFI paths are
  independent of the disk handle's lifetime.
- `disk.read(handle, sector, offset, length)` returns exactly `length`
  bytes, including embedded NUL bytes. The previous Lua implementation
  used a NUL-terminated string operation that could truncate binary data.
  `disk.write(handle, sector, offset, size, bytes)` rejects data shorter
  than the requested size.
- `video.fill_rect`, `video.draw_pixel` and `video.draw_string` accept
  the original table-based color argument through `GRUB_BIND_RECORD`.
  `video.bitmap_close` invalidates a bitmap handle and prevents double
  destruction; GC closes handles left open.
- The existing `disk.write` and `memrw.write_*` interfaces are **direct
  low-level block/memory operations**, not filesystem writes. They remain
  available only when their dedicated native modules are loaded. These
  operations are *not* subject to WriteFS mount gating, exactly as before.
  This is a distinct unsafe subsystem; do not conflate it with WriteFS.
- No general-purpose C symbol loader, arbitrary memory mapping interface,
  or implicit filesystem write access is introduced.

`writefs.*` remains in the Lua module and keeps its separate, explicitly
mounted writable filesystem namespace. The original GRUB read-only VFS does
not gain write access through the native binding registry.

## Verification

Run `tests/writefs_qemu.sh` on both configured build targets, with
`GRUB_PLATFORM=x86_64-efi` or `GRUB_PLATFORM=i386-pc`. Besides existing
WriteFS persistence tests it exercises native-first/Lua-first loading,
function invocation, missing argument rejection, multiple returns, handle
pinning, stale closures, module unloading and reloading. The EFI suite
additionally tests independent EFI Device Path lifetime. Raw disk write
regression runs only against disposable disk images, rewriting identical
bytes; raw physical memory is tested for interface and input validation,
not by writing unknown firmware memory.
