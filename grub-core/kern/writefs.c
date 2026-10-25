/* Common write commands and mount table. GPL-3.0-or-later. */
#include <grub/writefs.h>
#include <grub/command.h>
#include <grub/file.h>
#include <grub/env.h>
#include <grub/partition.h>
#include <grub/misc.h>
#include <grub/lockdown.h>

static struct grub_writefs_driver *drivers;
static struct grub_writefs_mount mounts[GRUB_WRITEFS_DRIVES];

/* Writable mounts are an explicit capability, and lockdown revokes it.
   Keep this check in Core: a Lua binding must not bypass command policy. */
static grub_err_t
require_writable (void)
{
  return grub_is_lockdown () == GRUB_LOCKDOWN_ENABLED
    ? grub_error (GRUB_ERR_ACCESS_DENIED,
                  "WriteFS modification is disabled during lockdown")
    : GRUB_ERR_NONE;
}

void grub_writefs_register (struct grub_writefs_driver *driver,
                            const char *types)
{
  driver->types = types;
  driver->next = drivers;
  drivers = driver;
}
void grub_writefs_unregister (struct grub_writefs_driver *driver)
{
  struct grub_writefs_driver **p;
  for (p = &drivers; *p; p = &(*p)->next)
    if (*p == driver) { *p = driver->next; return; }
}
struct grub_writefs_mount *grub_writefs_get (unsigned num)
{
  if (!num || num >= GRUB_WRITEFS_DRIVES || !mounts[num].disk)
    { grub_error (GRUB_ERR_BAD_DEVICE, "drive is not mounted"); return NULL; }
  return &mounts[num];
}
grub_err_t
grub_writefs_get_device (unsigned num, char **out)
{
  struct grub_writefs_mount *mount;
  char *part;
  if (!out) return grub_error (GRUB_ERR_BAD_ARGUMENT, "null device output");
  *out = NULL;
  mount = grub_writefs_get (num);
  if (!mount) return grub_errno;
  part = grub_partition_get_name (mount->disk->partition);
  if (!part) return grub_errno;
  *out = grub_xasprintf ("(%s%s%s)", mount->disk->name,
                         *part ? "," : "", part);
  grub_free (part);
  return *out ? GRUB_ERR_NONE : grub_errno;
}

grub_err_t
grub_writefs_get_label (unsigned num, char **out)
{
  struct grub_writefs_mount *mount;
  if (!out) return grub_error (GRUB_ERR_BAD_ARGUMENT, "null label output");
  *out = NULL;
  mount = grub_writefs_get (num);
  if (!mount) return grub_errno;
  if (!mount->driver->get_label)
    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET, "volume labels not supported");
  return mount->driver->get_label (mount, out);
}

grub_err_t
grub_writefs_set_label (unsigned num, const char *label)
{
  struct grub_writefs_mount *mount;
  if (require_writable ()) return grub_errno;
  if (!label) return grub_error (GRUB_ERR_BAD_ARGUMENT, "null volume label");
  mount = grub_writefs_get (num);
  if (!mount) return grub_errno;
  if (!mount->driver->set_label)
    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET, "volume labels not supported");
  return mount->driver->set_label (mount, label);
}

static int same_disk (grub_disk_t a, grub_disk_t b)
{
  return a->dev->id == b->dev->id && a->id == b->id &&
    grub_partition_get_start (a->partition) == grub_partition_get_start (b->partition) &&
    grub_disk_native_sectors (a) == grub_disk_native_sectors (b);
}

static struct grub_writefs_driver *
find_driver (grub_disk_t disk, const grub_uint8_t *boot)
{
  struct grub_writefs_driver *driver;
  for (driver = drivers; driver; driver = driver->next)
    if (driver->probe && driver->probe (disk, boot))
      return driver;
  grub_errno = GRUB_ERR_NONE;
  return NULL;
}

/* Write drivers announce themselves in writefs.lst at build time.  */
static struct grub_writefs_driver *
autoload_driver (grub_disk_t disk, const grub_uint8_t *boot)
{
  const char *prefix = grub_env_get ("prefix");
  struct grub_writefs_driver *driver = NULL;
  grub_file_t file;
  char *path;
  char name[128], c;
  grub_size_t len = 0;
  int overflow = 0;
  grub_ssize_t n;

  if (!prefix)
    return NULL;
  path = grub_xasprintf ("%s/" GRUB_TARGET_CPU "-" GRUB_PLATFORM
                         "/writefs.lst", prefix);
  if (!path)
    return NULL;
  file = grub_file_open (path, GRUB_FILE_TYPE_GRUB_MODULE_LIST);
  grub_free (path);
  if (!file)
    {
      grub_errno = GRUB_ERR_NONE;
      return NULL;
    }

  do
    {
      n = grub_file_read (file, &c, 1);
      if (n != 1 || c == '\n')
        {
          if (len && !overflow)
            {
              name[len] = 0;
              if (!grub_dl_get (name))
                grub_dl_load (name);
              grub_errno = GRUB_ERR_NONE;
              driver = find_driver (disk, boot);
              if (driver)
                break;
            }
          len = 0;
          overflow = 0;
        }
      else if (c != '\r')
        {
          if (len + 1 < sizeof (name))
            name[len++] = c;
          else
            overflow = 1;
        }
    }
  while (n == 1);
  grub_file_close (file);
  grub_errno = GRUB_ERR_NONE;
  return driver;
}

grub_err_t grub_writefs_mount (const char *name, unsigned num)
{
  struct grub_writefs_driver *driver;
  struct grub_writefs_mount *mount;
  grub_disk_t disk;
  grub_uint8_t boot[512];
  char *device;
  unsigned i;
  grub_err_t err;
  if (require_writable ()) return grub_errno;
  if (!num || num >= GRUB_WRITEFS_DRIVES)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid drive number");
  if (mounts[num].disk)
    return grub_error (GRUB_ERR_BAD_DEVICE, "drive number in use");
  device = grub_strdup (name);
  if (!device) return grub_errno;
  i = grub_strlen (device);
  if (i >= 2 && device[0] == '(' && device[i - 1] == ')')
    { device[i - 1] = 0; grub_memmove (device, device + 1, i - 1); }
  disk = grub_disk_open (device);
  grub_free (device);
  if (!disk) return grub_errno;
  for (i = 1; i < GRUB_WRITEFS_DRIVES; i++)
    if (mounts[i].disk && same_disk (disk, mounts[i].disk))
      { grub_error (GRUB_ERR_BAD_DEVICE, "volume already mounted"); goto fail; }
  if (grub_disk_read (disk, 0, 0, sizeof (boot), boot)) goto fail;
  driver = find_driver (disk, boot);
  if (!driver)
    driver = autoload_driver (disk, boot);
  if (!driver)
    { grub_error (GRUB_ERR_BAD_FS, "no writable filesystem driver found"); goto fail; }
  mount = &mounts[num];
  mount->disk = disk;
  mount->num = num;
  mount->driver = driver;
  err = driver->mount (mount);
  if (err)
    { grub_memset (mount, 0, sizeof (*mount)); goto fail; }
  grub_dl_ref (driver->mod);
  return GRUB_ERR_NONE;
fail:
  err = grub_errno;
  grub_disk_close (disk);
  return err;
}
grub_err_t grub_writefs_umount (unsigned num)
{
  struct grub_writefs_mount *mount = grub_writefs_get (num);
  grub_err_t err;
  grub_dl_t mod;
  if (!mount) return grub_errno;
  if (mount->open_files)
    return grub_error (GRUB_ERR_ACCESS_DENIED, "volume has open files");
  mod = mount->driver->mod;
  err = mount->driver->umount (mount);
  /* An unmount consumes backend state even when final I/O reports an error. */
  grub_disk_close (mount->disk);
  grub_memset (mount, 0, sizeof (*mount));
  grub_dl_unref (mod);
  return err;
}
static struct grub_writefs_mount *path_mount (const char *path)
{
  if (!path || path[0] < '1' || path[0] > '9' ||
      path[1] != ':' || path[2] != '/')
    { grub_error (GRUB_ERR_BAD_FILENAME, "expected N:/path"); return NULL; }
  return grub_writefs_get (path[0] - '0');
}
grub_err_t grub_writefs_open (struct grub_writefs_file *file, const char *path, unsigned flags)
{
  grub_err_t err;
  if (!file)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "null WriteFS file");
  grub_memset (file, 0, sizeof (*file));
  if ((flags & (GRUB_WRITEFS_WRITE | GRUB_WRITEFS_CREATE)) &&
      require_writable ())
    return grub_errno;
  if (!(flags & (GRUB_WRITEFS_READ | GRUB_WRITEFS_WRITE)) ||
      (flags & ~(GRUB_WRITEFS_READ | GRUB_WRITEFS_WRITE | GRUB_WRITEFS_CREATE)) ||
      ((flags & GRUB_WRITEFS_CREATE) && !(flags & GRUB_WRITEFS_WRITE)))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid WriteFS open flags");
  file->mount = path_mount (path);
  file->flags = flags;
  if (!file->mount) return grub_errno;
  err = file->mount->driver->open (file, path, flags);
  if (!err) file->mount->open_files++;
  else grub_memset (file, 0, sizeof (*file));
  return err;
}
grub_err_t grub_writefs_close (struct grub_writefs_file *file)
{
  grub_err_t err;
  if (!file || !file->mount)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "WriteFS file is closed");
  err = file->mount->driver->close (file);
  file->mount->open_files--;
  grub_memset (file, 0, sizeof (*file));
  return err;
}
grub_ssize_t
grub_writefs_read (struct grub_writefs_file *file, void *buf, grub_size_t len)
{
  if (!file || !file->mount || !(file->flags & GRUB_WRITEFS_READ) || (!buf && len))
    { grub_error (GRUB_ERR_BAD_ARGUMENT, "WriteFS file not open for reading"); return -1; }
  return len ? file->mount->driver->read (file, buf, len) : 0;
}

grub_ssize_t
grub_writefs_write (struct grub_writefs_file *file, const void *buf, grub_size_t len)
{
  if (!file || !file->mount || !(file->flags & GRUB_WRITEFS_WRITE) || (!buf && len))
    { grub_error (GRUB_ERR_BAD_ARGUMENT, "WriteFS file not open for writing"); return -1; }
  if (require_writable ()) return -1;
  return len ? file->mount->driver->write (file, buf, len) : 0;
}

grub_err_t
grub_writefs_seek (struct grub_writefs_file *file, grub_uint64_t offset)
{
  if (!file || !file->mount)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "WriteFS file is closed");
  if (offset > 0x7fffffffffffffffULL)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "WriteFS offset too large");
  file->offset = offset;
  return GRUB_ERR_NONE;
}

grub_err_t
grub_writefs_truncate (struct grub_writefs_file *file)
{
  if (!file || !file->mount || !(file->flags & GRUB_WRITEFS_WRITE))
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "WriteFS file not open for writing");
  if (require_writable ()) return grub_errno;
  return file->mount->driver->truncate (file);
}

grub_err_t
grub_writefs_mkdir (const char *path)
{
  if (require_writable ()) return grub_errno;
  struct grub_writefs_mount *mount = path_mount (path);
  return mount ? mount->driver->mkdir (mount, path) : grub_errno;
}

grub_err_t
grub_writefs_unlink (const char *path)
{
  if (require_writable ()) return grub_errno;
  struct grub_writefs_mount *mount = path_mount (path);
  return mount ? mount->driver->unlink (mount, path) : grub_errno;
}

grub_err_t
grub_writefs_touch (const char *path, const struct grub_datetime *dt,
                    unsigned count)
{
  if (require_writable ()) return grub_errno;
  struct grub_writefs_mount *mount = path_mount (path);
  return mount ? mount->driver->touch (mount, path, dt, count) : grub_errno;
}

/* Merge the installed manifest with dynamically registered backends.
   Enumeration never loads a writer and never mounts a volume. */
struct writefs_type
{
  struct writefs_type *next;
  char *type, *driver;
  int loaded;
};

struct writefs_types
{
  struct writefs_type *first, *last;
  grub_err_t err;
};

static int
collect_type (const char *type, const char *driver, int loaded, void *data)
{
  struct writefs_types *types = data;
  struct writefs_type *item;
  for (item = types->first; item; item = item->next)
    if (!grub_strcmp (item->type, type))
      {
        /* A loaded driver wins over the on-disk manifest's module hint. */
        if (loaded && !item->loaded)
          {
            char *name = grub_strdup (driver);
            if (!name) { types->err = grub_errno; return 1; }
            grub_free (item->driver);
            item->driver = name;
          }
        item->loaded |= loaded;
        return 0;
      }

  item = grub_zalloc (sizeof (*item));
  if (!item) { types->err = grub_errno; return 1; }
  item->type = grub_strdup (type);
  item->driver = grub_strdup (driver);
  if (!item->type || !item->driver)
    {
      types->err = grub_errno;
      grub_free (item->type);
      grub_free (item->driver);
      grub_free (item);
      return 1;
    }
  item->loaded = loaded;
  if (types->last) types->last->next = item;
  else types->first = item;
  types->last = item;
  return 0;
}

static void
free_types (struct writefs_type *item)
{
  while (item)
    {
      struct writefs_type *next = item->next;
      grub_free (item->type);
      grub_free (item->driver);
      grub_free (item);
      item = next;
    }
}

static int
report_types (const char *types, const char *driver, int loaded,
              grub_writefs_fs_hook_t hook, void *data)
{
  const char *p = types;
  while (p && *p)
    {
      char type[64];
      grub_size_t size = 0;
      int overflow = 0;
      while (*p && *p != ',')
        {
          if (size + 1 < sizeof (type)) type[size++] = *p;
          else overflow = 1;
          p++;
        }
      type[size] = 0;
      if (size && !overflow && hook (type, driver, loaded, data)) return 1;
      if (*p == ',') p++;
    }
  return 0;
}

grub_err_t
grub_writefs_iterate_filesystems (grub_writefs_fs_hook_t hook, void *data)
{
  const char *prefix = grub_env_get ("prefix");
  struct grub_writefs_driver *driver;
  struct writefs_types types = { 0 };
  struct writefs_type *entry;
  grub_file_t file = NULL;
  char *filename;
  char line[160], c;
  grub_size_t len = 0;
  grub_ssize_t n = 0;
  int overflow = 0;

  if (!hook)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "null WriteFS iterator");
  if (prefix)
    {
      filename = grub_xasprintf ("%s/" GRUB_TARGET_CPU "-" GRUB_PLATFORM
                                  "/writefs-types.lst", prefix);
      if (!filename) return grub_errno;
      file = grub_file_open (filename, GRUB_FILE_TYPE_GRUB_MODULE_LIST);
      grub_free (filename);
      if (!file) grub_errno = GRUB_ERR_NONE;
    }
  if (file)
    {
      do
        {
          n = grub_file_read (file, &c, 1);
          if (n != 1 || c == '\n')
            {
              if (len && !overflow && n >= 0)
                {
                  char *sep;
                  line[len] = 0;
                  sep = grub_strchr (line, ' ');
                  if (sep && sep[1])
                    {
                      *sep++ = 0;
                      if (collect_type (line, sep,
                                        grub_dl_get (sep) != NULL, &types))
                        break;
                    }
                }
              len = 0;
              overflow = 0;
            }
          else if (c != '\r')
            {
              if (len + 1 < sizeof (line)) line[len++] = c;
              else overflow = 1;
            }
        }
      while (n == 1);
      if (n < 0 && !types.err)
        types.err = grub_errno ? grub_errno
                   : grub_error (GRUB_ERR_FILE_READ_ERROR,
                                 "failed reading WriteFS manifest");
      {
        grub_err_t close_err = grub_file_close (file);
        if (!types.err) types.err = close_err;
      }
    }

  if (!types.err)
    for (driver = drivers; driver; driver = driver->next)
      if (report_types (driver->types, driver->name, 1,
                        collect_type, &types))
        break;

  if (!types.err)
    for (entry = types.first; entry; entry = entry->next)
      if (hook (entry->type, entry->driver, entry->loaded, data))
        break;
  {
    grub_err_t err = types.err;
    free_types (types.first);
    return err;
  }
}
static grub_err_t parse_num (const char *s, unsigned *num)
{
  if (s[0] < '1' || s[0] > '9' || s[1])
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected drive 1-9");
  *num = s[0] - '0';
  return GRUB_ERR_NONE;
}
static int
print_fs_type (const char *type, const char *driver, int loaded,
               void *data __attribute__ ((unused)))
{
  grub_printf ("%s: %s%s\n", type, driver, loaded ? " (loaded)" : "");
  return 0;
}

static grub_err_t cmd_mount (grub_command_t cmd __attribute__((unused)), int argc, char **args)
{
  unsigned num = 0;
  if (argc == 1 && (!grub_strcmp (args[0], "filesystems") ||
                    !grub_strcmp (args[0], "--list-filesystems")))
    return grub_writefs_iterate_filesystems (print_fs_type, NULL);
  if (argc == 1 && !grub_strcmp (args[0], "status"))
    {
      for (num = 1; num < GRUB_WRITEFS_DRIVES; num++)
        if (mounts[num].disk)
          {
            char *part = grub_partition_get_name (mounts[num].disk->partition);
            grub_printf ("(%s%s%s) -> %u: [%s]\n", mounts[num].disk->name,
                         part && *part ? "," : "", part ? part : "", num, mounts[num].driver->name);
            grub_free (part);
          }
      return GRUB_ERR_NONE;
    }
  if (argc != 2) return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected DISK NUM");
  if (parse_num (args[1], &num)) return grub_errno;
  return grub_writefs_mount (args[0], num);
}
static grub_err_t cmd_umount (grub_command_t cmd __attribute__((unused)), int argc, char **args)
{
  unsigned num = 0;
  if (argc != 1) return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected NUM");
  if (parse_num (args[0], &num)) return grub_errno;
  return grub_writefs_umount (num);
}
static grub_err_t cmd_path (grub_command_t cmd, int argc, char **args)
{
  if (argc != 1) return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected PATH");
  return !grub_strcmp (cmd->name, "mkdir") ?
    grub_writefs_mkdir (args[0]) : grub_writefs_unlink (args[0]);
}
grub_err_t grub_writefs_rename (const char *src, const char *dst)
{
  struct grub_writefs_mount *a, *b;
  if (require_writable ()) return grub_errno;
  a = path_mount (src);
  if (!a) return grub_errno;
  b = path_mount (dst);
  if (!b) return grub_errno;
  if (a != b) return grub_error (GRUB_ERR_BAD_ARGUMENT, "rename requires the same volume");
  return a->driver->rename (a, src, dst);
}
static grub_err_t copy_file (const char *src, const char *dst)
{
  struct grub_writefs_file in, out;
  grub_file_t native = NULL;
  grub_uint8_t *buf;
  grub_err_t err = GRUB_ERR_NONE, enderr;
  int mounted_src = src[0] >= '0' && src[0] <= '9' && src[1] == ':';
  grub_ssize_t nr, nw;
  if (mounted_src)
    { if (grub_writefs_open (&in, src, GRUB_WRITEFS_READ)) return grub_errno; }
  else
    {
      native = grub_file_open (src, GRUB_FILE_TYPE_HEXCAT | GRUB_FILE_TYPE_NO_DECOMPRESS);
      if (!native) return grub_errno;
      /* A native path into a mounted volume must share backend identity checks. */
      if (native->device && native->device->disk)
        {
          const char *path = src[0] == '(' ? grub_strchr (src, ')') + 1 : src;
          unsigned num;
          for (num = 1; path[0] == '/' && num < GRUB_WRITEFS_DRIVES; num++)
            if (mounts[num].disk && same_disk (native->device->disk, mounts[num].disk))
              {
                char *mounted_path = grub_xasprintf ("%u:%s", num, path);
                if (!mounted_path) { grub_file_close (native); return grub_errno; }
                err = grub_file_close (native);
                native = NULL;
                if (!err) err = grub_writefs_open (&in, mounted_path, GRUB_WRITEFS_READ);
                grub_free (mounted_path);
                if (err) return err;
                mounted_src = 1;
                break;
              }
        }
    }
  buf = grub_malloc (65536);
  if (!buf) { err = grub_errno; goto close_in; }
  if (grub_writefs_open (&out, dst, GRUB_WRITEFS_WRITE | GRUB_WRITEFS_CREATE))
    { err = grub_errno; goto free_buf; }
  if (mounted_src && in.mount == out.mount && in.id == out.id)
    { err = grub_error (GRUB_ERR_BAD_ARGUMENT, "source and destination are the same file"); goto close_out; }
  if (grub_writefs_truncate (&out)) { err = grub_errno; goto close_out; }
  for (;;)
    {
      nr = mounted_src ? grub_writefs_read (&in, buf, 65536) : grub_file_read (native, buf, 65536);
      if (nr < 0) { err = grub_errno; break; }
      if (!nr) break;
      nw = grub_writefs_write (&out, buf, nr);
      if (nw != nr)
        { err = grub_errno ? grub_errno : grub_error (GRUB_ERR_WRITE_ERROR, "short write"); break; }
    }
close_out:
  enderr = grub_writefs_close (&out);
  if (!err) err = enderr;
free_buf:
  grub_free (buf);
close_in:
  enderr = mounted_src ? grub_writefs_close (&in) : grub_file_close (native);
  return err ? err : enderr;
}
static grub_err_t cmd_pair (grub_command_t cmd, int argc, char **args)
{
  struct grub_writefs_mount *a, *b;
  grub_err_t err;
  if (argc != 2) return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected SOURCE DESTINATION");
  if (!grub_strcmp (cmd->name, "rename")) return grub_writefs_rename (args[0], args[1]);
  if (!grub_strcmp (cmd->name, "cp")) return copy_file (args[0], args[1]);
  a = path_mount (args[0]);
  if (!a) return grub_errno;
  b = path_mount (args[1]);
  if (!b) return grub_errno;
  if (a == b) return grub_writefs_rename (args[0], args[1]);
  err = copy_file (args[0], args[1]);
  return err ? err : grub_writefs_unlink (args[0]);
}
static grub_err_t cmd_touch (grub_command_t cmd __attribute__((unused)), int argc, char **args)
{
  struct grub_writefs_mount *mount;
  struct grub_datetime dt = { 0 };
  unsigned values[6] = { 0 }, i;
  const char *end;
  if (argc < 1 || argc > 7) return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected PATH [YEAR MONTH DAY HOUR MINUTE SECOND]");
  mount = path_mount (args[0]);
  if (!mount) return grub_errno;
  for (i = 1; i < (unsigned) argc; i++)
    {
      grub_uint64_t v = grub_strtoull (args[i], &end, 10);
      if (grub_errno || !*args[i] || *end || v > 65535)
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid date");
      values[i - 1] = v;
    }
  dt.year = values[0]; dt.month = values[1]; dt.day = values[2];
  dt.hour = values[3]; dt.minute = values[4]; dt.second = values[5];
  return grub_writefs_touch (args[0], &dt, argc - 1);
}
static grub_err_t cmd_write (grub_command_t cmd __attribute__((unused)), int argc, char **args)
{
  struct grub_writefs_file file;
  grub_uint64_t offset = 0;
  grub_ssize_t written;
  grub_size_t len;
  grub_err_t err, enderr;
  const char *end;
  if (argc != 2 && argc != 3) return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected FILE STRING [OFFSET]");
  if (argc == 3)
    {
      offset = grub_strtoull (args[2], &end, 0);
      if (grub_errno || !*args[2] || *end || offset > 0x7fffffffffffffffULL)
        return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid offset");
    }
  len = grub_strlen (args[1]);
  if (offset > 0x7fffffffffffffffULL - len) return grub_error (GRUB_ERR_OUT_OF_RANGE, "file offset overflow");
  if (grub_writefs_open (&file, args[0], GRUB_WRITEFS_WRITE)) return grub_errno;
  err = grub_writefs_seek (&file, offset);
  if (err) { grub_writefs_close (&file); return err; }
  written = grub_writefs_write (&file, args[1], len);
  err = written == (grub_ssize_t) len ? GRUB_ERR_NONE :
    (grub_errno ? grub_errno : grub_error (GRUB_ERR_WRITE_ERROR, "short write"));
  enderr = grub_writefs_close (&file);
  return err ? err : enderr;
}
void
grub_writefs_init (void)
{
  grub_register_command ("mount", cmd_mount, "DISK NUM | status | filesystems", "Mount a writable volume.");
  grub_register_command ("umount", cmd_umount, "NUM", "Unmount a writable volume.");
  grub_register_command ("mkdir", cmd_path, "PATH", "Create a directory.");
  grub_register_command ("rm", cmd_path, "PATH", "Remove a file or empty directory.");
  grub_register_command ("cp", cmd_pair, "SOURCE DESTINATION", "Copy a file.");
  grub_register_command ("mv", cmd_pair, "SOURCE DESTINATION", "Move a file.");
  grub_register_command ("rename", cmd_pair, "SOURCE DESTINATION", "Rename within a volume.");
  grub_register_command ("touch", cmd_touch, "PATH [YEAR MONTH DAY HOUR MINUTE SECOND]", "Create a file or change its timestamp.");
  grub_register_command ("write_file", cmd_write, "FILE STRING [OFFSET]", "Write a string to a file.");
}
