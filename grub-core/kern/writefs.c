/* Common write commands and mount table. GPL-3.0-or-later. */
#include <grub/writefs.h>
#include <grub/command.h>
#include <grub/file.h>
#include <grub/env.h>
#include <grub/partition.h>
#include <grub/misc.h>

static struct grub_writefs_driver *drivers;
static struct grub_writefs_mount mounts[GRUB_WRITEFS_DRIVES];

void grub_writefs_register (struct grub_writefs_driver *driver)
{
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
  if (path[0] < '1' || path[0] > '9' || path[1] != ':' || path[2] != '/')
    { grub_error (GRUB_ERR_BAD_FILENAME, "expected N:/path"); return NULL; }
  return grub_writefs_get (path[0] - '0');
}
static grub_err_t open_file (struct grub_writefs_file *file, const char *path, unsigned flags)
{
  grub_err_t err;
  grub_memset (file, 0, sizeof (*file));
  file->mount = path_mount (path);
  if (!file->mount) return grub_errno;
  err = file->mount->driver->open (file, path, flags);
  if (!err) file->mount->open_files++;
  return err;
}
static grub_err_t close_file (struct grub_writefs_file *file)
{
  grub_err_t err = file->mount->driver->close (file);
  file->mount->open_files--;
  return err;
}
static grub_err_t parse_num (const char *s, unsigned *num)
{
  if (s[0] < '1' || s[0] > '9' || s[1])
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected drive 1-9");
  *num = s[0] - '0';
  return GRUB_ERR_NONE;
}
static grub_err_t cmd_mount (grub_command_t cmd __attribute__((unused)), int argc, char **args)
{
  unsigned num = 0;
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
  struct grub_writefs_mount *mount;
  if (argc != 1) return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected PATH");
  mount = path_mount (args[0]);
  if (!mount) return grub_errno;
  if (!grub_strcmp (cmd->name, "mkdir")) return mount->driver->mkdir (mount, args[0]);
  return mount->driver->unlink (mount, args[0]);
}
static grub_err_t rename_file (const char *src, const char *dst)
{
  struct grub_writefs_mount *a = path_mount (src), *b;
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
    { if (open_file (&in, src, GRUB_WRITEFS_READ)) return grub_errno; }
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
                if (!err) err = open_file (&in, mounted_path, GRUB_WRITEFS_READ);
                grub_free (mounted_path);
                if (err) return err;
                mounted_src = 1;
                break;
              }
        }
    }
  buf = grub_malloc (65536);
  if (!buf) { err = grub_errno; goto close_in; }
  if (open_file (&out, dst, GRUB_WRITEFS_WRITE | GRUB_WRITEFS_CREATE))
    { err = grub_errno; goto free_buf; }
  if (mounted_src && in.mount == out.mount && in.id == out.id)
    { err = grub_error (GRUB_ERR_BAD_ARGUMENT, "source and destination are the same file"); goto close_out; }
  if (out.mount->driver->truncate (&out)) { err = grub_errno; goto close_out; }
  for (;;)
    {
      nr = mounted_src ? in.mount->driver->read (&in, buf, 65536) : grub_file_read (native, buf, 65536);
      if (nr < 0) { err = grub_errno; break; }
      if (!nr) break;
      nw = out.mount->driver->write (&out, buf, nr);
      if (nw != nr)
        { err = grub_errno ? grub_errno : grub_error (GRUB_ERR_WRITE_ERROR, "short write"); break; }
    }
close_out:
  enderr = close_file (&out);
  if (!err) err = enderr;
free_buf:
  grub_free (buf);
close_in:
  enderr = mounted_src ? close_file (&in) : grub_file_close (native);
  return err ? err : enderr;
}
static grub_err_t cmd_pair (grub_command_t cmd, int argc, char **args)
{
  struct grub_writefs_mount *a, *b;
  grub_err_t err;
  if (argc != 2) return grub_error (GRUB_ERR_BAD_ARGUMENT, "expected SOURCE DESTINATION");
  if (!grub_strcmp (cmd->name, "rename")) return rename_file (args[0], args[1]);
  if (!grub_strcmp (cmd->name, "cp")) return copy_file (args[0], args[1]);
  a = path_mount (args[0]);
  if (!a) return grub_errno;
  b = path_mount (args[1]);
  if (!b) return grub_errno;
  if (a == b) return rename_file (args[0], args[1]);
  err = copy_file (args[0], args[1]);
  return err ? err : a->driver->unlink (a, args[0]);
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
  return mount->driver->touch (mount, args[0], &dt, argc - 1);
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
  if (open_file (&file, args[0], GRUB_WRITEFS_WRITE)) return grub_errno;
  file.offset = offset;
  written = file.mount->driver->write (&file, args[1], len);
  err = written == (grub_ssize_t) len ? GRUB_ERR_NONE :
    (grub_errno ? grub_errno : grub_error (GRUB_ERR_WRITE_ERROR, "short write"));
  enderr = close_file (&file);
  return err ? err : enderr;
}
void
grub_writefs_init (void)
{
  grub_register_command ("mount", cmd_mount, "DISK NUM | status", "Mount a writable volume.");
  grub_register_command ("umount", cmd_umount, "NUM", "Unmount a writable volume.");
  grub_register_command ("mkdir", cmd_path, "PATH", "Create a directory.");
  grub_register_command ("rm", cmd_path, "PATH", "Remove a file or empty directory.");
  grub_register_command ("cp", cmd_pair, "SOURCE DESTINATION", "Copy a file.");
  grub_register_command ("mv", cmd_pair, "SOURCE DESTINATION", "Move a file.");
  grub_register_command ("rename", cmd_pair, "SOURCE DESTINATION", "Rename within a volume.");
  grub_register_command ("touch", cmd_touch, "PATH [YEAR MONTH DAY HOUR MINUTE SECOND]", "Create a file or change its timestamp.");
  grub_register_command ("write_file", cmd_write, "FILE STRING [OFFSET]", "Write a string to a file.");
}
