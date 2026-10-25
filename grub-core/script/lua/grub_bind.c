/* Lazy Lua adapter for the GRUB native binding ABI. GPL-3.0-or-later. */
#include "lua.h"
#include "lauxlib.h"
#include <grub/bind.h>
#include <grub/misc.h>

#define LUA_BIND_HANDLE "grub.bind.handle"

/* Notification callbacks may run during GRUB_MOD_INIT/FINI and must not
   enter Lua.  Registration is materialized only on script/field access. */
static int bindings_changed;
static struct grub_bind_runtime runtime;

void grub_lua_bind_attach (lua_State *state);
void grub_lua_bind_detach (void);
void grub_lua_bind_refresh (lua_State *state);

static int
bind_error (lua_State *state)
{
  char message[GRUB_MAX_ERRMSG];
  grub_strncpy (message, grub_errmsg, sizeof (message) - 1);
  message[sizeof (message) - 1] = 0;
  grub_errno = GRUB_ERR_NONE;
  return luaL_error (state, "%s", message);
}

static int
lua_bind_handle_gc (lua_State *state)
{
  struct grub_bind_handle **handle =
    luaL_checkudata (state, 1, LUA_BIND_HANDLE);
  if (*handle)
    {
      grub_bind_handle_release (*handle);
      *handle = NULL;
    }
  return 0;
}

static int
bind_argument (lua_State *state, int index, enum grub_bind_type type,
               struct grub_bind_value *out)
{
  const char *str, *end;
  grub_uint64_t u;
  int negative;
  out->type = type;
  switch (type)
    {
    case GRUB_BIND_NIL:
      if (!lua_isnil (state, index))
        return luaL_error (state, "nil expected");
      break;
    case GRUB_BIND_BOOL:
      if (!lua_isboolean (state, index))
        return luaL_error (state, "boolean expected");
      out->value.boolean = lua_toboolean (state, index);
      break;
    case GRUB_BIND_STRING:
    case GRUB_BIND_BYTES:
      if (lua_type (state, index) != LUA_TSTRING)
        return luaL_error (state, "string expected");
      str = lua_tolstring (state, index, &out->value.bytes.size);
      out->value.bytes.data = str;
      break;
    case GRUB_BIND_I64:
    case GRUB_BIND_U64:
      if (lua_type (state, index) == LUA_TNUMBER)
        {
          /* This GRUB Lua uses a 32-bit numeric representation. */
          lua_Integer v = lua_tointeger (state, index);
          if (type == GRUB_BIND_U64 && v < 0)
            return luaL_error (state, "negative unsigned integer");
          out->value.u64 = v;
          break;
        }
      if (lua_type (state, index) != LUA_TSTRING)
        return luaL_error (state, "integer or decimal string expected");
      str = lua_tostring (state, index);
      negative = *str == '-';
      if (negative)
        {
          if (type == GRUB_BIND_U64)
            return luaL_error (state, "negative unsigned integer");
          str++;
        }
      if (!*str || *str == '+' || *str == ' ' || *str == '\t')
        return luaL_error (state, "invalid decimal integer");
      grub_errno = GRUB_ERR_NONE;
      u = grub_strtoull (str, &end, 10);
      if (grub_errno)
        return bind_error (state);
      if (*end || (type == GRUB_BIND_I64 &&
                   u > (negative ? 0x8000000000000000ULL : 0x7fffffffffffffffULL)))
        return luaL_error (state, "integer out of range or malformed");
      if (negative)
        out->value.i64 = -(grub_int64_t) (u - 1) - 1;
      else
        out->value.u64 = u;
      break;
    case GRUB_BIND_HANDLE:
      {
        struct grub_bind_handle **handle =
          luaL_checkudata (state, index, LUA_BIND_HANDLE);
        if (!*handle)
          return luaL_error (state, "native handle was released");
        out->value.handle = *handle;
      }
      break;
    }
  return 0;
}

static void
push_result (lua_State *state, struct grub_bind_value *value)
{
  char buffer[40];
  grub_uint64_t u;
  switch (value->type)
    {
    case GRUB_BIND_NIL:
      lua_pushnil (state);
      break;
    case GRUB_BIND_BOOL:
      lua_pushboolean (state, value->value.boolean);
      break;
    case GRUB_BIND_I64:
      if (value->value.i64 >= -2147483648LL &&
          value->value.i64 <= 2147483647LL)
        lua_pushinteger (state, (int) value->value.i64);
      else
        {
          grub_snprintf (buffer, sizeof (buffer), "%lld",
                         (long long) value->value.i64);
          lua_pushstring (state, buffer);
        }
      break;
    case GRUB_BIND_U64:
      u = value->value.u64;
      if (u <= 2147483647ULL)
        lua_pushinteger (state, (int) u);
      else
        {
          grub_snprintf (buffer, sizeof (buffer), "%llu",
                         (unsigned long long) u);
          lua_pushstring (state, buffer);
        }
      break;
    case GRUB_BIND_STRING:
    case GRUB_BIND_BYTES:
      lua_pushlstring (state, value->value.bytes.data ?
                      value->value.bytes.data : "",
                      value->value.bytes.size);
      break;
    case GRUB_BIND_HANDLE:
      {
        struct grub_bind_handle **slot =
          lua_newuserdata (state, sizeof (*slot));
        *slot = value->value.handle;
        /* Ownership was transferred to Lua; the normal GC releases it. */
        value->value.handle = NULL;
        luaL_getmetatable (state, LUA_BIND_HANDLE);
        lua_setmetatable (state, -2);
      }
      break;
    }
}

static int
lua_bind_call (lua_State *state)
{
  grub_bind_id_t id = *(grub_bind_id_t *)
    lua_touserdata (state, lua_upvalueindex (1));
  const struct grub_bind_export *exported = grub_bind_describe (id);
  enum grub_bind_type param_types[GRUB_BIND_MAX_ARGS];
  unsigned min_args, max_args;
  struct grub_bind_value args[GRUB_BIND_MAX_ARGS];
  struct grub_bind_value results[GRUB_BIND_MAX_RESULTS];
  grub_size_t argc = lua_gettop (state), nresults = 0, i;
  grub_err_t err;

  if (!exported)
    return luaL_error (state, "native binding has been unloaded");
  /* Copy the signature before calling any VM operation that could run GC.
     Native code may be unloaded and reloaded between Lua invocations. */
  min_args = exported->min_args;
  max_args = exported->max_args;
  for (i = 0; i < max_args; i++)
    param_types[i] = exported->arg_types[i];
  if (argc < min_args || argc > max_args)
    return luaL_error (state, "incorrect native binding argument count");
  grub_memset (args, 0, sizeof (args));
  grub_memset (results, 0, sizeof (results));
  for (i = 0; i < argc; i++)
    bind_argument (state, i + 1, param_types[i], &args[i]);
  err = grub_bind_invoke (id, args, argc, results, &nresults);
  if (err)
    {
      for (i = 0; i < nresults; i++)
        grub_bind_value_dispose (&results[i]);
      return bind_error (state);
    }
  for (i = 0; i < nresults; i++)
    push_result (state, &results[i]);
  for (i = 0; i < nresults; i++)
    grub_bind_value_dispose (&results[i]);
  return nresults;
}

static int
lua_bind_namespace_index (lua_State *state)
{
  const char *name_space = lua_tostring (state, lua_upvalueindex (1));
  const char *name = lua_type (state, 2) == LUA_TSTRING ?
    lua_tostring (state, 2) : NULL;
  grub_bind_id_t id = name ? grub_bind_lookup (name_space, name) : 0;
  if (!id)
    {
      lua_pushnil (state);
      return 1;
    }
  grub_bind_id_t *slot = lua_newuserdata (state, sizeof (*slot));
  *slot = id;
  lua_pushcclosure (state, lua_bind_call, 1);
  return 1;
}

static void
create_namespace (lua_State *state, const char *name)
{
  /* Keep a stable namespace table; look up each method lazily so reload
     creates a new token and saved closures cannot call unloaded code. */
  lua_newtable (state);
  lua_newtable (state);
  lua_pushstring (state, name);
  lua_pushcclosure (state, lua_bind_namespace_index, 1);
  lua_setfield (state, -2, "__index");
  lua_setmetatable (state, -2);
}

static int
lua_bind_global_index (lua_State *state)
{
  const char *name = lua_type (state, 2) == LUA_TSTRING ?
    lua_tostring (state, 2) : NULL;

  if (name && grub_bind_has_namespace (name))
    {
      create_namespace (state, name);
      lua_pushvalue (state, 2);
      lua_pushvalue (state, -2);
      lua_rawset (state, LUA_GLOBALSINDEX);
      return 1;
    }
  /* Preserve a previous _G.__index handler when Lua had one. */
  if (lua_isfunction (state, lua_upvalueindex (1)))
    {
      lua_pushvalue (state, lua_upvalueindex (1));
      lua_pushvalue (state, 1);
      lua_pushvalue (state, 2);
      lua_call (state, 2, 1);
    }
  else if (lua_istable (state, lua_upvalueindex (1)))
    {
      lua_pushvalue (state, 2);
      lua_gettable (state, lua_upvalueindex (1));
    }
  else
    lua_pushnil (state);
  return 1;
}

static int
refresh_namespace (const struct grub_bind_export *exported,
                   grub_bind_id_t id __attribute__ ((unused)),
                   void *context)
{
  lua_State *state = context;
  lua_pushstring (state, exported->name_space);
  lua_rawget (state, LUA_GLOBALSINDEX);
  if (lua_isnil (state, -1))
    {
      lua_pop (state, 1);
      create_namespace (state, exported->name_space);
      lua_pushstring (state, exported->name_space);
      lua_pushvalue (state, -2);
      lua_rawset (state, LUA_GLOBALSINDEX);
    }
  lua_pop (state, 1);
  return 0;
}

static void
bindings_updated (void *context)
{
  int *dirty = context;
  *dirty = 1;
}

void
grub_lua_bind_refresh (lua_State *state)
{
  if (!state || !bindings_changed) return;
  bindings_changed = 0;
  grub_bind_iterate (refresh_namespace, state);
}

void
grub_lua_bind_attach (lua_State *state)
{
  luaL_newmetatable (state, LUA_BIND_HANDLE);
  lua_pushcfunction (state, lua_bind_handle_gc);
  lua_setfield (state, -2, "__gc");
  lua_pop (state, 1);

  if (!lua_getmetatable (state, LUA_GLOBALSINDEX))
    lua_newtable (state);
  lua_getfield (state, -1, "__index");
  lua_pushcclosure (state, lua_bind_global_index, 1);
  lua_setfield (state, -2, "__index");
  lua_setmetatable (state, LUA_GLOBALSINDEX);

  bindings_changed = 1;
  runtime.changed = bindings_updated;
  runtime.context = &bindings_changed;
  grub_bind_runtime_attach (&runtime);
}

void
grub_lua_bind_detach (void)
{
  grub_bind_runtime_detach (&runtime);
  bindings_changed = 0;
}
