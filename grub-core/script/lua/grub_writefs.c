/* Lua API for the explicit-mount WriteFS subsystem. GPL-3.0-or-later. */
#include <grub/writefs.h>
#include <grub/misc.h>
#include "lua.h"
#include "lauxlib.h"

#define LUA_WRITEFS_FILE "grub.writefs.file"

void grub_lua_register_writefs (lua_State *state);

struct lua_writefs_handle
{
  struct grub_writefs_file file;
};

static struct grub_writefs_file *
check_file (lua_State *state)
{
  struct lua_writefs_handle *h =
    luaL_checkudata (state, 1, LUA_WRITEFS_FILE);
  if (!h->file.mount)
    luaL_error (state, "WriteFS file is closed");
  return &h->file;
}

static int
writefs_error (lua_State *state)
{
  char message[GRUB_MAX_ERRMSG];
  grub_strncpy (message, grub_errmsg, sizeof (message) - 1);
  message[sizeof (message) - 1] = 0;
  /* luaL_error longjmps; a caught Lua error must not poison GRUB I/O. */
  grub_errno = GRUB_ERR_NONE;
  return luaL_error (state, "%s", message);
}

static int
lua_writefs_mount (lua_State *state)
{
  const char *disk = luaL_checkstring (state, 1);
  int num = luaL_checkinteger (state, 2);
  if (num < 1 || num >= GRUB_WRITEFS_DRIVES)
    return luaL_error (state, "invalid WriteFS mount number");
  if (grub_writefs_mount (disk, num))
    return writefs_error (state);
  return 0;
}

static int
lua_writefs_umount (lua_State *state)
{
  int num = luaL_checkinteger (state, 1);
  if (num < 1 || num >= GRUB_WRITEFS_DRIVES)
    return luaL_error (state, "invalid WriteFS mount number");
  if (grub_writefs_umount (num))
    return writefs_error (state);
  return 0;
}

static int
lua_writefs_open (lua_State *state)
{
  const char *path = luaL_checkstring (state, 1);
  int flags = luaL_optinteger (state, 2, GRUB_WRITEFS_READ);
  struct lua_writefs_handle *h;
  h = lua_newuserdata (state, sizeof (*h));
  grub_memset (h, 0, sizeof (*h));
  if (grub_writefs_open (&h->file, path, flags))
    return writefs_error (state);
  luaL_getmetatable (state, LUA_WRITEFS_FILE);
  lua_setmetatable (state, -2);
  return 1;
}

static int
lua_writefs_close (lua_State *state)
{
  struct grub_writefs_file *file = check_file (state);
  if (grub_writefs_close (file))
    return writefs_error (state);
  return 0;
}

static int
lua_writefs_gc (lua_State *state)
{
  struct lua_writefs_handle *h =
    luaL_checkudata (state, 1, LUA_WRITEFS_FILE);
  if (h->file.mount)
    grub_writefs_close (&h->file);
  return 0;
}

static int
lua_writefs_read (lua_State *state)
{
  struct grub_writefs_file *file = check_file (state);
  lua_Integer wanted = luaL_checkinteger (state, 2);
  luaL_Buffer output;
  if (wanted < 0)
    return luaL_error (state, "negative read length");
  luaL_buffinit (state, &output);
  while (wanted > 0)
    {
      char *buf = luaL_prepbuffer (&output);
      grub_size_t size = wanted > LUAL_BUFFERSIZE ?
        LUAL_BUFFERSIZE : (grub_size_t) wanted;
      grub_ssize_t nr = grub_writefs_read (file, buf, size);
      if (nr < 0)
        return writefs_error (state);
      if (!nr) break;
      luaL_addsize (&output, nr);
      wanted -= nr;
    }
  luaL_pushresult (&output);
  return 1;
}

static int
lua_writefs_write (lua_State *state)
{
  struct grub_writefs_file *file = check_file (state);
  grub_size_t len;
  const char *data = luaL_checklstring (state, 2, &len);
  grub_ssize_t nw = grub_writefs_write (file, data, len);
  if (nw < 0) return writefs_error (state);
  lua_pushinteger (state, nw);
  return 1;
}

static int
lua_writefs_seek (lua_State *state)
{
  struct grub_writefs_file *file = check_file (state);
  lua_Integer offset = luaL_checkinteger (state, 2);
  if (offset < 0 || grub_writefs_seek (file, offset))
    return luaL_error (state, "invalid WriteFS offset");
  return 0;
}

static int
lua_writefs_tell (lua_State *state)
{
  struct grub_writefs_file *file = check_file (state);
  lua_pushinteger (state, file->offset);
  return 1;
}

static int
lua_writefs_size (lua_State *state)
{
  struct grub_writefs_file *file = check_file (state);
  lua_pushinteger (state, file->size);
  return 1;
}

static int
lua_writefs_eof (lua_State *state)
{
  struct grub_writefs_file *file = check_file (state);
  lua_pushboolean (state, file->offset >= file->size);
  return 1;
}

static int
lua_writefs_truncate (lua_State *state)
{
  if (grub_writefs_truncate (check_file (state)))
    return writefs_error (state);
  return 0;
}

static int
lua_writefs_mkdir (lua_State *state)
{
  if (grub_writefs_mkdir (luaL_checkstring (state, 1)))
    return writefs_error (state);
  return 0;
}

static int
lua_writefs_unlink (lua_State *state)
{
  if (grub_writefs_unlink (luaL_checkstring (state, 1)))
    return writefs_error (state);
  return 0;
}

static int
lua_writefs_rename (lua_State *state)
{
  const char *src = luaL_checkstring (state, 1);
  const char *dst = luaL_checkstring (state, 2);
  if (grub_writefs_rename (src, dst))
    return writefs_error (state);
  return 0;
}

static int
lua_writefs_touch (lua_State *state)
{
  const char *path = luaL_checkstring (state, 1);
  struct grub_datetime dt = { 0 };
  unsigned vals[6] = { 0 };
  unsigned i, count = lua_gettop (state) - 1;
  if (count > 6)
    return luaL_error (state, "expected up to six date fields");
  for (i = 0; i < count; i++)
    {
      lua_Integer val = luaL_checkinteger (state, i + 2);
      if (val < 0 || val > 65535)
        return luaL_error (state, "invalid date value");
      vals[i] = val;
    }
  dt.year = vals[0]; dt.month = vals[1]; dt.day = vals[2];
  dt.hour = vals[3]; dt.minute = vals[4]; dt.second = vals[5];
  if (grub_writefs_touch (path, &dt, count))
    return writefs_error (state);
  return 0;
}

struct lua_writefs_iter
{
  lua_State *state;
  int count;
};

static int
lua_writefs_type_hook (const char *type, const char *driver, int loaded,
                       void *context)
{
  struct lua_writefs_iter *iter = context;
  lua_State *state = iter->state;
  lua_newtable (state);
  lua_pushstring (state, type);
  lua_setfield (state, -2, "name");
  lua_pushstring (state, driver);
  lua_setfield (state, -2, "driver");
  lua_pushboolean (state, loaded);
  lua_setfield (state, -2, "loaded");
  lua_rawseti (state, -2, ++iter->count);
  return 0;
}

static int
lua_writefs_supported (lua_State *state)
{
  struct lua_writefs_iter iter = { state, 0 };
  lua_newtable (state);
  if (grub_writefs_iterate_filesystems (lua_writefs_type_hook, &iter))
    return writefs_error (state);
  return 1;
}

static const luaL_Reg writefs_lib[] =
{
  {"mount", lua_writefs_mount},
  {"umount", lua_writefs_umount},
  {"supported", lua_writefs_supported},
  {"open", lua_writefs_open},
  {"close", lua_writefs_close},
  {"read", lua_writefs_read},
  {"write", lua_writefs_write},
  {"seek", lua_writefs_seek},
  {"tell", lua_writefs_tell},
  {"size", lua_writefs_size},
  {"eof", lua_writefs_eof},
  {"truncate", lua_writefs_truncate},
  {"mkdir", lua_writefs_mkdir},
  {"unlink", lua_writefs_unlink},
  {"rename", lua_writefs_rename},
  {"touch", lua_writefs_touch},
  {NULL, NULL}
};

void
grub_lua_register_writefs (lua_State *state)
{
  luaL_newmetatable (state, LUA_WRITEFS_FILE);
  lua_pushcfunction (state, lua_writefs_gc);
  lua_setfield (state, -2, "__gc");
  lua_pop (state, 1);
  luaL_register (state, "writefs", writefs_lib);
  lua_pop (state, 1);
}
