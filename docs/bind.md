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
string, bytes (both with explicit lengths) and managed resource handles.
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

`writefs.*` remains in the Lua module and keeps its explicit writable mount
and independent path namespace. The original GRUB read-only VFS does not gain
write access through the native binding registry.

The `dd`, `memrw`, `videoinfo` and EFI `dp` modules still have legacy Lua
bindings. Their raw disk, memory, bitmap and firmware object semantics need
separate ownership and capability analysis before migration. Their functions
are not automatically exported by the registry.

## Verification

Run `tests/writefs_qemu.sh` on both configured build targets, with
`GRUB_PLATFORM=x86_64-efi` or `GRUB_PLATFORM=i386-pc`. Besides existing
WriteFS persistence tests it exercises native-first/Lua-first loading,
function invocation, missing argument rejection, multiple returns, handle
pinning, stale closures, module unloading and reloading.
