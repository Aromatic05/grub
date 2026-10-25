/* ntfs-3g backend for GRUB writefs. GPL-3.0-or-later. */
#include "grub_ntfs_config.h"
#include <grub/writefs.h>
#include "volume.h"
#include "device.h"
#include "dir.h"
#include "attrib.h"
#include "inode.h"
#include "unistr.h"
#include "ntfstime.h"

GRUB_MOD_LICENSE ("GPLv3+");
struct ntfs_grub_device { grub_disk_t disk; s64 offset; int failed; };
extern struct ntfs_device_operations ntfs_grub_io_ops;
struct ntfs_file
{
  ntfs_inode *inode;
  ntfs_attr *attr;
  struct ntfs_file *next;
};
struct ntfs_mount
{
  struct ntfs_grub_device io;
  ntfs_volume *vol;
  struct ntfs_file *files;
};
struct target { char *parent; ntfschar *name; int len; };
static struct grub_writefs_driver driver;
static int backend_probe (grub_disk_t disk __attribute__ ((unused)),
                          const grub_uint8_t *boot)
{ return grub_memcmp (boot + 3, "NTFS    ", 8) == 0; }

static grub_err_t ntfs_error (const char *operation)
{
  return grub_error (errno == ENOENT ? GRUB_ERR_FILE_NOT_FOUND :
                     errno == ENOMEM ? GRUB_ERR_OUT_OF_MEMORY : GRUB_ERR_WRITE_ERROR,
                     "NTFS %s failed (errno %d)", operation, errno);
}
static grub_err_t backend_mount (struct grub_writefs_mount *mount)
{
  struct ntfs_mount *state;
  struct ntfs_device *dev;
  grub_uint64_t sectors = grub_disk_native_sectors (mount->disk);
  if (sectors == GRUB_DISK_SIZE_UNKNOWN || sectors > (0x7fffffffffffffffULL >> 9))
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "NTFS device size out of range");
  state = grub_zalloc (sizeof (*state));
  if (!state) return grub_errno;
  state->io.disk = mount->disk;
  dev = ntfs_device_alloc (mount->disk->name, 0, &ntfs_grub_io_ops, &state->io);
  if (!dev) { grub_free (state); return ntfs_error ("device allocation"); }
  /* No recovery, hibernation override, or read-only downgrade on a write mount. */
  state->vol = ntfs_device_mount (dev, NTFS_MNT_EXCLUSIVE);
  if (!state->vol)
    { int saved = errno; ntfs_device_free (dev); grub_free (state); errno = saved; return ntfs_error ("mount"); }
  if (NVolReadOnly (state->vol) || (state->vol->flags & VOLUME_IS_DIRTY))
    {
      ntfs_umount (state->vol, FALSE);
      grub_free (state);
      return grub_error (GRUB_ERR_ACCESS_DENIED, "NTFS volume is read-only or dirty");
    }
  NVolClearCaseSensitive (state->vol);
  if (ntfs_volume_get_free_space (state->vol))
    { int saved = errno; ntfs_umount (state->vol, FALSE); grub_free (state); errno = saved; return ntfs_error ("free space"); }
  mount->data = state;
  return GRUB_ERR_NONE;
}
static grub_err_t backend_umount (struct grub_writefs_mount *mount)
{
  struct ntfs_mount *state = mount->data;
  int res = ntfs_umount (state->vol, FALSE);
  grub_free (state);
  return res ? ntfs_error ("unmount") : GRUB_ERR_NONE;
}
static void target_free (struct target *target)
{ grub_free (target->parent); free (target->name); }
static int target_parse (ntfs_volume *vol, const char *path, struct target *target)
{
  char *last;
  memset (target, 0, sizeof (*target));
  target->parent = strdup (path + 2);
  if (!target->parent) { errno = ENOMEM; return -1; }
  last = strrchr (target->parent, '/');
  if (!last || !last[1]) { errno = EINVAL; goto fail; }
  target->len = ntfs_mbstoucs (last + 1, &target->name);
  if (target->len <= 0 || target->len > 255)
    { errno = EINVAL; goto fail; }
  if (ntfs_forbidden_names (vol, target->name, target->len, TRUE))
    { errno = EINVAL; goto fail; }
  if (last == target->parent) last[1] = 0;
  else *last = 0;
  return 0;
fail:
  target_free (target);
  return -1;
}
static int mutable_inode (ntfs_inode *ni)
{
  if (ni->mft_no < FILE_first_user || ni->flags & (FILE_ATTR_READONLY | FILE_ATTR_REPARSE_POINT | FILE_ATTR_ENCRYPTED))
    { errno = EPERM; return -1; }
  return 0;
}
static ntfs_inode *create_inode (struct ntfs_mount *state, const char *path, mode_t type)
{
  struct target target;
  ntfs_inode *parent, *ni;
  int saved;
  if (target_parse (state->vol, path, &target)) return NULL;
  parent = ntfs_pathname_to_inode (state->vol, NULL, target.parent);
  if (!parent) { target_free (&target); return NULL; }
  ni = ntfs_create (parent, 0, target.name, target.len, type);
  saved = errno;
  if (ni && ntfs_inode_sync (ni)) { saved = errno; ntfs_inode_close (ni); ni = NULL; }
  if (ntfs_inode_close (parent) && ni) { saved = errno; ntfs_inode_close (ni); ni = NULL; }
  target_free (&target);
  errno = saved;
  return ni;
}
static grub_err_t backend_open (struct grub_writefs_file *file, const char *path, unsigned flags)
{
  struct ntfs_mount *state = file->mount->data;
  struct ntfs_file *fp, *other;
  ntfs_inode *ni = ntfs_pathname_to_inode (state->vol, NULL, path + 2);
  if (!ni && errno == ENOENT && (flags & GRUB_WRITEFS_CREATE))
    ni = create_inode (state, path, S_IFREG);
  if (!ni) return ntfs_error ("open");
  if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)
    { errno = EISDIR; goto fail; }
  if (ni->flags & (FILE_ATTR_REPARSE_POINT | FILE_ATTR_ENCRYPTED))
    { errno = EOPNOTSUPP; goto fail; }
  if ((flags & GRUB_WRITEFS_WRITE) && mutable_inode (ni)) goto fail;
  /* ntfs-3g requires a single live inode object for each MFT record. */
  for (other = state->files; other; other = other->next)
    if (other->inode->mft_no == ni->mft_no)
      { errno = EBUSY; goto fail; }
  fp = grub_zalloc (sizeof (*fp));
  if (!fp) { errno = ENOMEM; goto fail; }
  fp->attr = ntfs_attr_open (ni, AT_DATA, AT_UNNAMED, 0);
  if (!fp->attr) { grub_free (fp); goto fail; }
  fp->inode = ni;
  fp->next = state->files;
  state->files = fp;
  file->data = fp;
  file->size = fp->attr->data_size;
  file->id = ni->mft_no;
  return GRUB_ERR_NONE;
fail:
  { int saved = errno; ntfs_inode_close (ni); errno = saved; }
  return ntfs_error ("open");
}
static grub_ssize_t backend_read (struct grub_writefs_file *file, void *buf, grub_size_t len)
{
  struct ntfs_file *fp = file->data;
  s64 n;
  if (file->offset >= (u64) fp->attr->data_size) return 0;
  if (len > (u64) fp->attr->data_size - file->offset) len = fp->attr->data_size - file->offset;
  n = ntfs_attr_pread (fp->attr, file->offset, len, buf);
  if (n < 0) { ntfs_error ("read"); return -1; }
  file->offset += n;
  return n;
}
static grub_ssize_t backend_write (struct grub_writefs_file *file, const void *buf, grub_size_t len)
{
  struct ntfs_file *fp = file->data;
  grub_size_t done = 0;
  while (done < len)
    {
      s64 n = ntfs_attr_pwrite (fp->attr, file->offset, len - done, (const u8 *) buf + done);
      if (n <= 0)
        { if (!n) errno = EIO; ntfs_error ("write"); return -1; }
      done += n;
      file->offset += n;
    }
  file->size = fp->attr->data_size;
  ntfs_inode_update_times (fp->inode, NTFS_UPDATE_MCTIME);
  return done;
}
static grub_err_t backend_truncate (struct grub_writefs_file *file)
{
  struct ntfs_file *fp = file->data;
  if (ntfs_attr_truncate (fp->attr, file->offset)) return ntfs_error ("truncate");
  ntfs_inode_update_times (fp->inode, NTFS_UPDATE_MCTIME);
  file->size = fp->attr->data_size;
  return GRUB_ERR_NONE;
}
static grub_err_t backend_close (struct grub_writefs_file *file)
{
  struct ntfs_mount *state = file->mount->data;
  struct ntfs_file *fp = file->data, **p;
  int res;
  for (p = &state->files; *p; p = &(*p)->next)
    if (*p == fp) { *p = fp->next; break; }
  ntfs_attr_close (fp->attr);
  res = ntfs_inode_close (fp->inode);
  grub_free (fp);
  return res ? ntfs_error ("close") : GRUB_ERR_NONE;
}
static grub_err_t backend_mkdir (struct grub_writefs_mount *mount, const char *path)
{
  ntfs_inode *ni = create_inode (mount->data, path, S_IFDIR);
  if (!ni) return ntfs_error ("mkdir");
  return ntfs_inode_close (ni) ? ntfs_error ("mkdir close") : GRUB_ERR_NONE;
}
static grub_err_t backend_unlink (struct grub_writefs_mount *mount, const char *path)
{
  struct ntfs_mount *state = mount->data;
  struct target target;
  ntfs_inode *ni, *parent;
  int res, saved;
  if (target_parse (state->vol, path, &target)) return ntfs_error ("unlink path");
  ni = ntfs_pathname_to_inode (state->vol, NULL, path + 2);
  if (!ni) { target_free (&target); return ntfs_error ("unlink"); }
  if (mutable_inode (ni) || ntfs_check_empty_dir (ni)) goto fail;
  parent = ntfs_pathname_to_inode (state->vol, NULL, target.parent);
  if (!parent) goto fail;
  res = ntfs_delete (state->vol, path + 2, ni, parent, target.name, target.len);
  target_free (&target);
  return res ? ntfs_error ("unlink") : GRUB_ERR_NONE;
fail:
  saved = errno; ntfs_inode_close (ni); target_free (&target); errno = saved;
  return ntfs_error ("unlink");
}
static grub_err_t backend_rename (struct grub_writefs_mount *mount, const char *src, const char *dst)
{
  struct ntfs_mount *state = mount->data;
  struct target old, new;
  ntfs_inode *ni = NULL, *parent = NULL, *ancestor;
  u64 source_id, parent_id;
  int res, saved, is_dir;
  if (target_parse (state->vol, src, &old)) return ntfs_error ("rename source");
  if (target_parse (state->vol, dst, &new)) { target_free (&old); return ntfs_error ("rename destination"); }
  ni = ntfs_pathname_to_inode (state->vol, NULL, src + 2);
  if (!ni || mutable_inode (ni)) goto fail;
  source_id = ni->mft_no;
  is_dir = !!(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY);
  res = ntfs_inode_close (ni); ni = NULL;
  if (res) goto fail;
  parent = ntfs_pathname_to_inode (state->vol, NULL, new.parent);
  if (!parent) goto fail;
  parent_id = parent->mft_no;
  if (ntfs_inode_lookup_by_name (parent, new.name, new.len) != (u64) -1)
    { errno = EEXIST; goto fail; }
  if (errno != ENOENT) goto fail;
  /* Check ancestors before creating a directory link, including case aliases. */
  if (is_dir)
    {
      while (parent->mft_no != FILE_root)
        {
          if (parent->mft_no == source_id) { errno = EINVAL; goto fail; }
          ancestor = ntfs_dir_parent_inode (parent);
          res = ntfs_inode_close (parent); parent = ancestor;
          if (res || !parent) goto fail;
        }
    }
  res = ntfs_inode_close (parent); parent = NULL;
  if (res) goto fail;
  ni = ntfs_inode_open (state->vol, source_id);
  if (!ni) goto fail;
  parent = ntfs_inode_open (state->vol, parent_id);
  if (!parent) goto fail;
  if (ntfs_link (ni, parent, new.name, new.len)) goto fail;
  res = ntfs_inode_close_in_dir (ni, parent); ni = NULL;
  saved = errno;
  if (ntfs_inode_close (parent) && !res) { saved = errno; res = -1; }
  parent = NULL; errno = saved;
  if (res) goto fail;
  /* Reopen only after both linked inode and destination directory are synced. */
  ni = ntfs_inode_open (state->vol, source_id);
  if (!ni) goto fail;
  parent = ntfs_pathname_to_inode (state->vol, NULL, old.parent);
  if (!parent) goto fail;
  res = ntfs_delete (state->vol, src + 2, ni, parent, old.name, old.len);
  target_free (&old); target_free (&new);
  return res ? ntfs_error ("rename unlink") : GRUB_ERR_NONE;
fail:
  saved = errno;
  if (ni) ntfs_inode_close (ni);
  if (parent) ntfs_inode_close (parent);
  target_free (&old); target_free (&new); errno = saved;
  return ntfs_error ("rename");
}
static grub_err_t backend_touch (struct grub_writefs_mount *mount, const char *path, const struct grub_datetime *values, unsigned count)
{
  struct ntfs_mount *state = mount->data;
  struct grub_datetime resolved;
  const struct grub_datetime *dt = &resolved;
  ntfs_inode *ni;
  struct timespec ts;
  if (grub_writefs_touch_datetime (values, count, &resolved))
    return grub_errno;
  ni = ntfs_pathname_to_inode (state->vol, NULL, path + 2);
  if (!ni && errno == ENOENT) ni = create_inode (state, path, S_IFREG);
  if (!ni) return ntfs_error ("touch");
  if (mutable_inode (ni))
    { int saved = errno; ntfs_inode_close (ni); errno = saved; return ntfs_error ("touch"); }
  grub_datetime2unixtime (dt, &ts.tv_sec); ts.tv_nsec = 0;
  ni->last_data_change_time = timespec2ntfs (ts);
  ni->last_access_time = timespec2ntfs (ts);
  ni->last_mft_change_time = ntfs_current_time ();
  NInoSetDirty (ni);
  NInoFileNameSetDirty (ni);
  return ntfs_inode_close (ni) ? ntfs_error ("touch close") : GRUB_ERR_NONE;
}
static struct grub_writefs_driver driver = {
  .name = "ntfs3g", .probe = backend_probe,
  .mount = backend_mount, .umount = backend_umount,
  .open = backend_open, .read = backend_read, .write = backend_write,
  .truncate = backend_truncate, .close = backend_close,
  .mkdir = backend_mkdir, .unlink = backend_unlink,
  .rename = backend_rename, .touch = backend_touch
};
GRUB_MOD_INIT(ntfs3g)
{
  driver.mod = mod;
  grub_writefs_register (&driver, "ntfs");
}
GRUB_MOD_FINI(ntfs3g)
{
  grub_writefs_unregister (&driver);
}
