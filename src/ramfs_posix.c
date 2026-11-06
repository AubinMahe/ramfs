#include "ramfs_core.h"
#include "ramfs_vfs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
typedef intptr_t ssize_t;
#else
#include <unistd.h>
#endif

/* =========================================================================
 * Structures internes de navigation de répertoires - privées au module POSIX
 * ========================================================================= */

struct rfs_dir {
   DIR *real_dir;
   struct rfs_dirent *entries;
   size_t count;
   size_t current_idx;
   struct rfs_dirent current;
};

/* =========================================================================
 * Opérations sur les descripteurs de fichiers (POSIX fd)
 * ========================================================================= */

static rfs_handle_t allocate_access_slot(const ram_file_t *file, int access_mode) {
   rfs_handle_t free_slot = RFS_INVALID_HANDLE;
   for (unsigned i = 0; i < g_access_capacity; i++) {
      const ram_file_access_t *access = g_access_table + i;
      if (access->file == file) {
         int active_mode = access->flags & O_ACCMODE;
         if (active_mode != O_RDONLY || access_mode != O_RDONLY) {
            errno = EBUSY;
            return RFS_INVALID_HANDLE;
         }
      } else if ((free_slot == RFS_INVALID_HANDLE) && (access->file == NULL)) {
         free_slot = (rfs_handle_t)i;
      }
   }
   if (free_slot == RFS_INVALID_HANDLE) {
      unsigned old_cap = g_access_capacity;
      if (expand_access_table() != 0) {
         fprintf(stderr, "Capacité d'accès atteinte !\n");
         errno = ENOMEM;
         return RFS_INVALID_HANDLE;
      }
      free_slot = (rfs_handle_t)old_cap;
   }
   return free_slot;
}

rfs_handle_t rfs_open(const char *pathname, int flags, ...) {
   if ((g_files == NULL) || (g_access_table == NULL)) {
      errno = ENODEV;
      return RFS_INVALID_HANDLE;
   }
   if (!pathname) {
      errno = EINVAL;
      return RFS_INVALID_HANDLE;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return RFS_INVALID_HANDLE;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
      mode_t mode = 0;
      if (flags & O_CREAT) {
         va_list ap;
         va_start(ap, flags);
         mode = (mode_t)va_arg(ap, int);
         va_end(ap);
      }
#undef open
      return (flags & O_CREAT) ? open(pathname, flags, mode) : open(pathname, flags);
   }

   if (ram_rel[0] == '\0' || strcmp(ram_rel, ".") == 0 || strcmp(ram_rel, "/") == 0) {
      errno = EISDIR;
      return RFS_INVALID_HANDLE;
   }
   int access_mode = flags & O_ACCMODE;
   size_t hash = hash_path(ram_rel);
   rfs_catalog_wrlock();
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash);
   ram_file_t *file = NULL;
   if (entry) {
      file = entry->file;
      if (file->is_dir) {
         rfs_catalog_unlock();
         errno = EISDIR;
         return RFS_INVALID_HANDLE;
      }
      if ((flags & O_CREAT) && (flags & O_EXCL)) {
         rfs_catalog_unlock();
         errno = EEXIST;
         return RFS_INVALID_HANDLE;
      }
   } else {
      if (is_dir_prefix(ram_rel)) {
         rfs_catalog_unlock();
         errno = EISDIR;
         return RFS_INVALID_HANDLE;
      }
      if (flags & O_CREAT) {
         entry = create_entry_in_catalog(ram_rel, hash);
         if (entry == NULL) {
            rfs_catalog_unlock();
            return RFS_INVALID_HANDLE;
         }
         file = entry->file;
      } else {
         rfs_catalog_unlock();
         errno = ENOENT;
         return RFS_INVALID_HANDLE;
      }
   }
   rfs_catalog_unlock();

   rfs_access_lock();
   rfs_handle_t free_slot = allocate_access_slot(file, access_mode);
   if (free_slot == RFS_INVALID_HANDLE) {
      rfs_access_unlock();
      return RFS_INVALID_HANDLE;
   }
   ram_shared_offset_t *so = malloc(sizeof(ram_shared_offset_t));
   if (!so) {
      rfs_access_unlock();
      errno = ENOMEM;
      return RFS_INVALID_HANDLE;
   }
   RFS_MUTEX_INIT(&so->lock);
   rfs_file_rdlock(file);
   so->offset = (flags & O_APPEND) ? (off_t)file->size : 0;
   rfs_file_unlock(file);
   so->ref_count = 1;

   if (flags & O_TRUNC) {
      rfs_file_wrlock(file);
      rfs_file_truncate(file, 0);
      rfs_file_unlock(file);
   }
   ram_file_access_t *access = g_access_table + free_slot;
   access->file = file;
   access->shared_offset = so;
   access->flags = flags;
   access->eof = 0;
   access->error = 0;
   access->is_tmpfile = 0;
   g_access_used++;
   if (g_access_used > g_peak_access) {
      g_peak_access = g_access_used;
   }
   rfs_access_unlock();
   return g_mount_len > 0 ? (rfs_handle_t)(RFS_FD_BASE + free_slot) : free_slot;
}

ssize_t rfs_read(rfs_handle_t fd, void *buf, size_t count) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   if (!is_rfs_fd(fd)) {
      if (g_mount_len > 0) {
#undef read
         return read(fd, buf, count);
      }
      errno = EBADF;
      return -1;
   }
   rfs_access_lock();
   ram_file_access_t *acc = get_access(fd);
   if (!acc || (acc->flags & O_ACCMODE) == O_WRONLY) {
      if (acc) {
         acc->error = 1;
         errno = EBADF;
      }
      rfs_access_unlock();
      return -1;
   }
   ram_file_t *file = acc->file;
   off_t cur_off = acc->shared_offset->offset;
   rfs_access_unlock();

   rfs_file_rdlock(file);
   if (cur_off >= (off_t)file->size) {
      rfs_file_unlock(file);
      if (count > 0) {
         rfs_access_lock();
         acc->eof = 1;
         rfs_access_unlock();
      }
      return 0;
   }
   size_t n = rfs_file_read_bytes(file, (size_t)cur_off, buf, count);
   rfs_file_unlock(file);

   rfs_access_lock();
   acc->shared_offset->offset += (off_t)n;
   if (n < count) {
      acc->eof = 1;
   }
   rfs_access_unlock();
   return (ssize_t)n;
}

ssize_t rfs_write(rfs_handle_t fd, const void *buf, size_t count) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   if (!is_rfs_fd(fd)) {
      if (g_mount_len > 0) {
#undef write
         return write(fd, buf, count);
      }
      errno = EBADF;
      return -1;
   }
   rfs_access_lock();
   ram_file_access_t *acc = get_access(fd);
   if (!acc || (acc->flags & O_ACCMODE) == O_RDONLY) {
      if (acc) {
         acc->error = 1;
         errno = EBADF;
      }
      rfs_access_unlock();
      return -1;
   }
   ram_file_t *file = acc->file;
   int is_append = (acc->flags & O_APPEND);
   off_t cur_off = acc->shared_offset->offset;
   rfs_access_unlock();

   rfs_file_wrlock(file);
   if (is_append) {
      cur_off = (off_t)file->size;
   }
   if (count == 0) {
      rfs_file_unlock(file);
      return 0;
   }
   if (rfs_file_write_bytes(file, (size_t)cur_off, buf, count) != 0) {
      rfs_file_unlock(file);
      rfs_access_lock();
      acc->error = 1;
      rfs_access_unlock();
      errno = ENOMEM;
      return -1;
   }
   rfs_file_unlock(file);

   rfs_access_lock();
   acc->shared_offset->offset = cur_off + (off_t)count;
   rfs_access_unlock();
   return (ssize_t)count;
}

off_t rfs_lseek(rfs_handle_t fd, off_t offset, int whence) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   if (!is_rfs_fd(fd)) {
      if (g_mount_len > 0) {
#undef lseek
         return lseek(fd, offset, whence);
      }
      errno = EBADF;
      return -1;
   }
   rfs_access_lock();
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      rfs_access_unlock();
      return -1;
   }
   ram_file_t *file = acc->file;
   rfs_file_rdlock(file);
   off_t cur_size = (off_t)file->size;
   rfs_file_unlock(file);

   off_t new_offset = acc->shared_offset->offset;
   switch (whence) {
   case SEEK_SET:
      new_offset = offset;
      break;
   case SEEK_CUR:
      new_offset += offset;
      break;
   case SEEK_END:
      new_offset = cur_size + offset;
      break;
   default:
      rfs_access_unlock();
      errno = EINVAL;
      return -1;
   }
   if (new_offset < 0) {
      rfs_access_unlock();
      errno = EINVAL;
      return -1;
   }
   acc->shared_offset->offset = new_offset;
   acc->eof = 0;
   rfs_access_unlock();
   return new_offset;
}

int rfs_close(rfs_handle_t fd) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   if (!is_rfs_fd(fd)) {
      if (g_mount_len > 0) {
#undef close
         return close(fd);
      }
      errno = EBADF;
      return -1;
   }
   rfs_access_lock();
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      rfs_access_unlock();
      return -1;
   }
   if (acc->shared_offset) {
      if (--acc->shared_offset->ref_count == 0) {
         RFS_MUTEX_DESTROY(&acc->shared_offset->lock);
         free(acc->shared_offset);
      }
      acc->shared_offset = NULL;
   }
   memset(acc, 0, sizeof(ram_file_access_t));
   if (g_access_used > 0) {
      g_access_used--;
   }
   rfs_access_unlock();
   return 0;
}

int rfs_unlink(const char *pathname) {
   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
#undef unlink
      return unlink(pathname);
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   if (ram_rel[0] == '\0' || strcmp(ram_rel, ".") == 0 || strcmp(ram_rel, "/") == 0) {
      errno = EISDIR;
      return -1;
   }
   rfs_catalog_wrlock();
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash_path(ram_rel));
   if (entry == NULL) {
      if (is_dir_prefix(ram_rel)) {
         rfs_catalog_unlock();
         errno = EISDIR;
         return -1;
      }
      rfs_catalog_unlock();
      errno = ENOENT;
      return -1;
   }
   ram_file_t *file = entry->file;
   if (file->is_dir) {
      rfs_catalog_unlock();
      errno = EISDIR;
      return -1;
   }
   rfs_access_lock();
   for (unsigned i = 0; i < g_access_capacity; i++) {
      if (g_access_table[i].file == file) {
         rfs_access_unlock();
         rfs_catalog_unlock();
         errno = EBUSY;
         return -1;
      }
   }
   rfs_access_unlock();

   rfs_file_free_storage(file);
   rfs_file_lock_destroy(file);
   unsigned index = (unsigned)(file - g_files);
   memset(file, 0, sizeof(ram_file_t));
   rfs_file_lock_init(file);
   g_free_stack[g_free_top++] = index;
   remove_from_catalog((size_t)(entry - g_table));
   if (g_files_used > 0) {
      g_files_used--;
   }
   rfs_catalog_unlock();
   return 0;
}

int rfs_remove(const char *pathname) {
   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
#undef remove
      return remove(pathname);
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash_path(ram_rel));
   if ((entry && entry->file && entry->file->is_dir) || is_dir_prefix(ram_rel)) {
      return rfs_rmdir(pathname);
   }
   return rfs_unlink(pathname);
}

int rfs_stat(const char *pathname, struct stat *buf) {
   if (!pathname || !buf) {
      errno = EINVAL;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
#undef stat
      return stat(pathname, buf);
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   rfs_catalog_rdlock();
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash_path(ram_rel));
   if (entry && entry->file) {
      rfs_file_rdlock(entry->file);
      fill_stat_buf(entry->file, buf);
      rfs_file_unlock(entry->file);
      rfs_catalog_unlock();
      return 0;
   }
   if (ram_rel[0] == '\0' || strcmp(ram_rel, ".") == 0 || strcmp(ram_rel, "/") == 0 || is_dir_prefix(ram_rel)) {
      fill_stat_buf(NULL, buf);
      buf->st_mode = S_IFDIR | 0755;
      buf->st_nlink = 2;
      rfs_catalog_unlock();
      return 0;
   }
   rfs_catalog_unlock();
   errno = ENOENT;
   return -1;
}

int rfs_fstat(rfs_handle_t fd, struct stat *buf) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   if (!buf) {
      errno = EINVAL;
      return -1;
   }
   if (!is_rfs_fd(fd)) {
      if (g_mount_len > 0) {
#undef fstat
         return fstat(fd, buf);
      }
      errno = EBADF;
      return -1;
   }
   rfs_access_lock();
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      rfs_access_unlock();
      return -1;
   }
   ram_file_t *file = acc->file;
   rfs_access_unlock();

   rfs_file_rdlock(file);
   fill_stat_buf(file, buf);
   rfs_file_unlock(file);
   return 0;
}

static int check_rename_target(const ram_file_t *old_file, const char *ram_new, ram_catalog_t *new_entry) {
   if (!new_entry || !new_entry->file) {
      return 0;
   }
   if (new_entry->file->is_dir != old_file->is_dir) {
      errno = old_file->is_dir ? ENOTDIR : EISDIR;
      return -1;
   }
   if (new_entry->file->is_dir && is_dir_prefix(ram_new)) {
      errno = ENOTEMPTY;
      return -1;
   }
   for (unsigned i = 0; i < g_access_capacity; i++) {
      if (g_access_table[i].file == new_entry->file) {
         errno = EBUSY;
         return -1;
      }
   }
   return 0;
}

static void free_overwritten_entry(ram_catalog_t *new_entry) {
   if (!new_entry || !new_entry->file) {
      return;
   }
   rfs_file_free_storage(new_entry->file);
   unsigned index = (unsigned)(new_entry->file - g_files);
   memset(new_entry->file, 0, sizeof(ram_file_t));
   g_free_stack[g_free_top++] = index;
   remove_from_catalog((size_t)(new_entry - g_table));
   if (g_files_used > 0) {
      g_files_used--;
   }
}

static void rename_directory_children(const char *ram_old, const char *ram_new, const ram_file_t *old_file) {
   size_t old_len = strlen(ram_old);
   for (size_t idx = 0; idx <= g_table_mask; idx++) {
      ram_catalog_t *e = g_table + idx;
      if (e->path != NULL && e->file != NULL && e->file != old_file) {
         if (strncmp(e->path, ram_old, old_len) == 0 && e->path[old_len] == '/') {
            char child_new[PATH_MAX];
            snprintf(child_new, sizeof(child_new), "%s%s", ram_new, e->path + old_len);
            char *child_copy = strdup(child_new);
            if (child_copy) {
               ram_file_t *cf = e->file;
               remove_from_catalog(idx);
               size_t chash = hash_path(child_new);
               size_t pos = chash & g_table_mask;
               while (g_table[pos].path != NULL) {
                  pos = (pos + 1) & g_table_mask;
               }
               g_table[pos].path = child_copy;
               g_table[pos].file = cf;
               g_table[pos].hash = chash;
               catalog_depth_add(child_new);
               idx = (size_t)-1;
            }
         }
      }
   }
}

int rfs_rename(const char *oldpath, const char *newpath) {
   if (!oldpath || !newpath) {
      errno = EINVAL;
      return -1;
   }
   char clean_old[PATH_MAX];
   char clean_new[PATH_MAX];
   if (resolve_path(oldpath, clean_old, sizeof(clean_old)) != 0 || resolve_path(newpath, clean_new, sizeof(clean_new)) != 0) {
      return -1;
   }
   char ram_old[PATH_MAX];
   char ram_new[PATH_MAX];
   int old_under = is_under_mount(clean_old, ram_old, sizeof(ram_old));
   int new_under = is_under_mount(clean_new, ram_new, sizeof(ram_new));
   if (!old_under && !new_under) {
#undef rename
      return rename(oldpath, newpath);
   }
   if (old_under != new_under) {
      errno = EXDEV;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   rfs_catalog_wrlock();
   if (strcmp(ram_old, ram_new) == 0) {
      ram_catalog_t *e = search_in_catalog(ram_old, hash_path(ram_old));
      if (!e || !e->file) {
         rfs_catalog_unlock();
         errno = ENOENT;
         return -1;
      }
      rfs_catalog_unlock();
      return 0;
   }
   ram_catalog_t *old_entry = search_in_catalog(ram_old, hash_path(ram_old));
   if (!old_entry || !old_entry->file) {
      rfs_catalog_unlock();
      errno = ENOENT;
      return -1;
   }
   ram_file_t *old_file = old_entry->file;

   ram_catalog_t *new_entry = search_in_catalog(ram_new, hash_path(ram_new));
   if (check_rename_target(old_file, ram_new, new_entry) != 0) {
      rfs_catalog_unlock();
      return -1;
   }

   char *new_path_copy = strdup(ram_new);
   if (!new_path_copy) {
      rfs_catalog_unlock();
      errno = ENOMEM;
      return -1;
   }

   free_overwritten_entry(new_entry);

   old_entry = search_in_catalog(ram_old, hash_path(ram_old));
   remove_from_catalog((size_t)(old_entry - g_table));

   size_t hash = hash_path(ram_new);
   size_t i = hash & g_table_mask;
   while (g_table[i].path != NULL) {
      i = (i + 1) & g_table_mask;
   }
   g_table[i].path = new_path_copy;
   g_table[i].file = old_file;
   g_table[i].hash = hash;
   catalog_depth_add(ram_new);

   if (old_file->is_dir) {
      rename_directory_children(ram_old, ram_new, old_file);
   }

   rfs_catalog_unlock();
   return 0;
}

int rfs_access(const char *pathname, int mode) {
   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
#undef access
      return access(pathname, mode);
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash_path(ram_rel));
   int is_dir = 0;
   if (entry && entry->file) {
      is_dir = entry->file->is_dir;
   } else if (ram_rel[0] == '\0' || strcmp(ram_rel, ".") == 0 || strcmp(ram_rel, "/") == 0 || is_dir_prefix(ram_rel)) {
      is_dir = 1;
   } else {
      errno = ENOENT;
      return -1;
   }
   if (!is_dir && (mode & 1 /* X_OK */)) {
      errno = EACCES;
      return -1;
   }
   return 0;
}

int rfs_mkdir(const char *pathname, ...) {
   mode_t mode = 0755;
   va_list ap;
   va_start(ap, pathname);
   mode = (mode_t)va_arg(ap, int);
   va_end(ap);
   (void)mode;

   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
#undef mkdir
#ifdef _WIN32
      return mkdir(pathname);
#else
      return mkdir(pathname, mode);
#endif
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   if (ram_rel[0] == '\0' || strcmp(ram_rel, ".") == 0 || strcmp(ram_rel, "/") == 0) {
      errno = EEXIST;
      return -1;
   }
   size_t hash = hash_path(ram_rel);
   rfs_catalog_wrlock();
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash);
   if (entry != NULL || is_dir_prefix(ram_rel)) {
      rfs_catalog_unlock();
      errno = EEXIST;
      return -1;
   }
   const char *last_slash = strrchr(ram_rel, '/');
   if (last_slash) {
      char parent[PATH_MAX];
      size_t plen = (size_t)(last_slash - ram_rel);
      if (plen > 0) {
         memcpy(parent, ram_rel, plen);
         parent[plen] = '\0';
         ram_catalog_t *pe = search_in_catalog(parent, hash_path(parent));
         if (pe) {
            if (!pe->file->is_dir) {
               rfs_catalog_unlock();
               errno = ENOTDIR;
               return -1;
            }
         } else if (!is_dir_prefix(parent)) {
            rfs_catalog_unlock();
            errno = ENOENT;
            return -1;
         }
      }
   }
   if (!create_dir_entry_in_catalog(ram_rel, hash)) {
      rfs_catalog_unlock();
      return -1;
   }
   rfs_catalog_unlock();
   return 0;
}

int rfs_rmdir(const char *pathname) {
   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
#undef rmdir
      return rmdir(pathname);
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   if (ram_rel[0] == '\0' || strcmp(ram_rel, ".") == 0 || strcmp(ram_rel, "/") == 0) {
      errno = EBUSY;
      return -1;
   }
   rfs_catalog_wrlock();
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash_path(ram_rel));
   if (!entry || !entry->file) {
      if (is_dir_prefix(ram_rel)) {
         rfs_catalog_unlock();
         errno = ENOTEMPTY;
         return -1;
      }
      rfs_catalog_unlock();
      errno = ENOENT;
      return -1;
   }
   if (!entry->file->is_dir) {
      rfs_catalog_unlock();
      errno = ENOTDIR;
      return -1;
   }
   size_t len = strlen(ram_rel);
   for (size_t i = 0; i <= g_table_mask; i++) {
      const ram_catalog_t *e = g_table + i;
      if (e->path != NULL && e->file != NULL && e != entry) {
         if (strncmp(e->path, ram_rel, len) == 0 && e->path[len] == '/') {
            rfs_catalog_unlock();
            errno = ENOTEMPTY;
            return -1;
         }
      }
   }
   unsigned index = (unsigned)(entry->file - g_files);
   memset(entry->file, 0, sizeof(ram_file_t));
   g_free_stack[g_free_top++] = index;
   remove_from_catalog((size_t)(entry - g_table));
   if (g_files_used > 0) {
      g_files_used--;
   }
   rfs_catalog_unlock();
   return 0;
}

int rfs_isatty(rfs_handle_t fd) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return 0;
   }
   if (!is_rfs_fd(fd)) {
      if (g_mount_len > 0) {
#undef isatty
#ifdef _WIN32
         return _isatty(fd);
#else
         return isatty(fd);
#endif
      }
      errno = EBADF;
      return 0;
   }
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      errno = EBADF;
      return 0;
   }
   errno = ENOTTY;
   return 0;
}

int rfs_dup(rfs_handle_t oldfd) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   if (!is_rfs_fd(oldfd)) {
      if (g_mount_len > 0) {
#undef dup
         return dup(oldfd);
      }
      errno = EBADF;
      return -1;
   }
   rfs_access_lock();
   if (!get_access(oldfd)) {
      rfs_access_unlock();
      return -1;
   }
   unsigned old_slot = fd_to_slot(oldfd);
   rfs_handle_t free_slot = RFS_INVALID_HANDLE;
   for (unsigned i = 0; i < g_access_capacity; i++) {
      if (g_access_table[i].file == NULL) {
         free_slot = (rfs_handle_t)i;
         break;
      }
   }
   if (free_slot == RFS_INVALID_HANDLE) {
      unsigned old_cap = g_access_capacity;
      if (expand_access_table() != 0) {
         rfs_access_unlock();
         errno = EMFILE;
         return -1;
      }
      free_slot = (rfs_handle_t)old_cap;
   }
   ram_file_access_t *old_acc = g_access_table + old_slot;
   ram_file_access_t *new_acc = g_access_table + free_slot;
   new_acc->file = old_acc->file;
   new_acc->shared_offset = old_acc->shared_offset;
   new_acc->shared_offset->ref_count++;
   new_acc->flags = old_acc->flags;
   new_acc->eof = old_acc->eof;
   new_acc->error = old_acc->error;
   new_acc->is_tmpfile = old_acc->is_tmpfile;
   g_access_used++;
   if (g_access_used > g_peak_access) {
      g_peak_access = g_access_used;
   }
   rfs_access_unlock();
   return g_mount_len > 0 ? (int)(RFS_FD_BASE + free_slot) : (int)free_slot;
}

int rfs_dup2(rfs_handle_t oldfd, rfs_handle_t newfd) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   if (!is_rfs_fd(oldfd)) {
      if (g_mount_len > 0) {
#undef dup2
         return dup2(oldfd, newfd);
      }
      errno = EBADF;
      return -1;
   }
   rfs_access_lock();
   ram_file_access_t *old_acc = get_access(oldfd);
   if (!old_acc) {
      rfs_access_unlock();
      return -1;
   }
   if (oldfd == newfd) {
      rfs_access_unlock();
      return newfd;
   }
   unsigned target_slot = fd_to_slot(newfd);
   while (target_slot >= g_access_capacity) {
      if (expand_access_table() != 0) {
         rfs_access_unlock();
         errno = EBADF;
         return -1;
      }
   }
   if (g_access_table[target_slot].file != NULL) {
      ram_file_access_t *tacc = g_access_table + target_slot;
      if (tacc->shared_offset) {
         if (--tacc->shared_offset->ref_count == 0) {
            RFS_MUTEX_DESTROY(&tacc->shared_offset->lock);
            free(tacc->shared_offset);
         }
         tacc->shared_offset = NULL;
      }
      memset(tacc, 0, sizeof(ram_file_access_t));
      if (g_access_used > 0) {
         g_access_used--;
      }
   }
   unsigned old_slot = fd_to_slot(oldfd);
   old_acc = g_access_table + old_slot;
   ram_file_access_t *new_acc = g_access_table + target_slot;
   new_acc->file = old_acc->file;
   new_acc->shared_offset = old_acc->shared_offset;
   new_acc->shared_offset->ref_count++;
   new_acc->flags = old_acc->flags;
   new_acc->eof = old_acc->eof;
   new_acc->error = old_acc->error;
   new_acc->is_tmpfile = old_acc->is_tmpfile;
   g_access_used++;
   if (g_access_used > g_peak_access) {
      g_peak_access = g_access_used;
   }
   rfs_access_unlock();
   return newfd;
}

int rfs_ftruncate(rfs_handle_t fd, off_t length) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   if (length < 0) {
      errno = EINVAL;
      return -1;
   }
   if (!is_rfs_fd(fd)) {
      if (g_mount_len > 0) {
#undef ftruncate
#ifdef _WIN32
         return _chsize(fd, (long)length);
#else
         return ftruncate(fd, length);
#endif
      }
      errno = EBADF;
      return -1;
   }
   rfs_access_lock();
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      rfs_access_unlock();
      return -1;
   }
   if ((acc->flags & O_ACCMODE) == O_RDONLY) {
      rfs_access_unlock();
      errno = EINVAL;
      return -1;
   }
   ram_file_t *file = acc->file;
   rfs_access_unlock();

   rfs_file_wrlock(file);
   int res = rfs_file_truncate(file, (size_t)length);
   rfs_file_unlock(file);
   return res;
}

int rfs_truncate(const char *pathname, off_t length) {
   if (!pathname || length < 0) {
      errno = EINVAL;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
#undef truncate
#ifdef _WIN32
      int fd = _open(pathname, _O_RDWR);
      if (fd < 0) {
         return -1;
      }
      int ret = _chsize(fd, (long)length);
      _close(fd);
      return ret;
#else
      return truncate(pathname, length);
#endif
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   rfs_catalog_rdlock();
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash_path(ram_rel));
   if (!entry || !entry->file) {
      if (is_dir_prefix(ram_rel)) {
         rfs_catalog_unlock();
         errno = EISDIR;
         return -1;
      }
      rfs_catalog_unlock();
      errno = ENOENT;
      return -1;
   }
   ram_file_t *file = entry->file;
   if (file->is_dir) {
      rfs_catalog_unlock();
      errno = EISDIR;
      return -1;
   }
   rfs_catalog_unlock();

   rfs_file_wrlock(file);
   int res = rfs_file_truncate(file, (size_t)length);
   rfs_file_unlock(file);
   return res;
}

/* =========================================================================
 * Opérations sur les répertoires (dirent POSIX)
 * ========================================================================= */

static int add_dirent(struct rfs_dirent **entries, size_t *count, size_t *cap, ino_t ino, unsigned char type, const char *name) {
   for (size_t i = 0; i < *count; i++) {
      if (strcmp((*entries)[i].d_name, name) == 0) {
         return 0;
      }
   }
   if (*count >= *cap) {
      size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
      struct rfs_dirent *new_arr = realloc(*entries, new_cap * sizeof(struct rfs_dirent));
      if (!new_arr) {
         return -1;
      }
      *entries = new_arr;
      *cap = new_cap;
   }
   struct rfs_dirent *ent = &(*entries)[*count];
   ent->d_ino = ino;
   ent->d_type = type;
   strncpy(ent->d_name, name, sizeof(ent->d_name) - 1);
   ent->d_name[sizeof(ent->d_name) - 1] = '\0';
   (*count)++;
   return 0;
}

static rfs_dir_t *open_host_dir(const char *name) {
#undef opendir
   DIR *rd = opendir(name);
   if (!rd) {
      return NULL;
   }
   rfs_dir_t *d = calloc(1, sizeof(rfs_dir_t));
   if (!d) {
      closedir(rd);
      errno = ENOMEM;
      return NULL;
   }
   d->real_dir = rd;
   return d;
}

static int validate_ram_dir(const char *ram_rel, int is_root) {
   if (is_root) {
      return 0;
   }
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash_path(ram_rel));
   if (entry) {
      if (!entry->file->is_dir) {
         errno = ENOTDIR;
         return -1;
      }
      return 0;
   }
   if (!is_dir_prefix(ram_rel)) {
      errno = ENOENT;
      return -1;
   }
   return 0;
}

static int add_catalog_dirent(rfs_dir_t *dir, size_t *cap, const ram_catalog_t *e, const char *sub) {
   const char *slash = strchr(sub, '/');
   if (!slash) {
      unsigned char type = e->file->is_dir ? DT_DIR : DT_REG;
      ino_t ino = (ino_t)((size_t)(e->file - g_files) + 10);
      return add_dirent(&dir->entries, &dir->count, cap, ino, type, sub);
   }
   char dir_name[256];
   size_t comp_len = (size_t)(slash - sub);
   if (comp_len >= sizeof(dir_name)) {
      comp_len = sizeof(dir_name) - 1;
   }
   memcpy(dir_name, sub, comp_len);
   dir_name[comp_len] = '\0';
   return add_dirent(&dir->entries, &dir->count, cap, 3, DT_DIR, dir_name);
}

static int collect_dir_children(rfs_dir_t *dir, size_t *cap, const char *prefix, size_t prefix_len, int is_root) {
   for (size_t i = 0; i <= g_table_mask; i++) {
      const ram_catalog_t *e = g_table + i;
      if (!e->path || !e->file) {
         continue;
      }
      const char *sub = NULL;
      if (is_root) {
         const char *p = (e->path[0] == '/') ? e->path + 1 : e->path;
         if (p[0] == '\0' || strcmp(p, ".") == 0) {
            continue;
         }
         sub = p;
      } else if (strncmp(e->path, prefix, prefix_len) == 0) {
         sub = e->path + prefix_len;
      }

      if (sub && sub[0] != '\0') {
         if (add_catalog_dirent(dir, cap, e, sub) != 0) {
            return -1;
         }
      }
   }
   return 0;
}

rfs_dir_t *rfs_opendir(const char *name) {
   if (!name) {
      errno = EINVAL;
      return NULL;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(name, clean_path, sizeof(clean_path)) != 0) {
      return NULL;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
      return open_host_dir(name);
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return NULL;
   }

   rfs_catalog_rdlock();
   int is_root = (ram_rel[0] == '\0' || strcmp(ram_rel, ".") == 0 || strcmp(ram_rel, "/") == 0);
   if (validate_ram_dir(ram_rel, is_root) != 0) {
      rfs_catalog_unlock();
      return NULL;
   }

   rfs_dir_t *dir = calloc(1, sizeof(rfs_dir_t));
   if (!dir) {
      rfs_catalog_unlock();
      errno = ENOMEM;
      return NULL;
   }

   size_t cap = 0;
   if (add_dirent(&dir->entries, &dir->count, &cap, 1, DT_DIR, ".") != 0 ||
       add_dirent(&dir->entries, &dir->count, &cap, 2, DT_DIR, "..") != 0) {
      rfs_catalog_unlock();
      free(dir->entries);
      free(dir);
      errno = ENOMEM;
      return NULL;
   }

   char prefix[PATH_MAX + 2];
   size_t prefix_len = 0;
   if (!is_root) {
      size_t clen = strlen(ram_rel);
      if (clen + 2 > sizeof(prefix)) {
         rfs_catalog_unlock();
         free(dir->entries);
         free(dir);
         errno = ENAMETOOLONG;
         return NULL;
      }
      memcpy(prefix, ram_rel, clen);
      prefix[clen] = '/';
      prefix[clen + 1] = '\0';
      prefix_len = clen + 1;
   }

   if (collect_dir_children(dir, &cap, prefix, prefix_len, is_root) != 0) {
      rfs_catalog_unlock();
      free(dir->entries);
      free(dir);
      errno = ENOMEM;
      return NULL;
   }
   rfs_catalog_unlock();

   return dir;
}

struct rfs_dirent *rfs_readdir(rfs_dir_t *dirp) {
   if (!dirp) {
      errno = EBADF;
      return NULL;
   }
   if (dirp->real_dir != NULL) {
#undef readdir
      struct dirent *de = readdir(dirp->real_dir);
      if (!de) {
         return NULL;
      }
      dirp->current.d_ino = de->d_ino;
#if defined(_DIRENT_HAVE_D_TYPE) || defined(__linux__) || defined(__APPLE__)
      dirp->current.d_type = de->d_type;
#else
      dirp->current.d_type = DT_UNKNOWN;
#endif
      snprintf(dirp->current.d_name, sizeof(dirp->current.d_name), "%s", de->d_name);
      return &dirp->current;
   }
   if (!g_files || !g_table) {
      errno = EBADF;
      return NULL;
   }
   if (dirp->current_idx >= dirp->count) {
      return NULL;
   }
   dirp->current = dirp->entries[dirp->current_idx++];
   return &dirp->current;
}

int rfs_closedir(rfs_dir_t *dirp) {
   if (!dirp) {
      errno = EBADF;
      return -1;
   }
   if (dirp->real_dir != NULL) {
#undef closedir
      int ret = closedir(dirp->real_dir);
      free(dirp);
      return ret;
   }
   free(dirp->entries);
   free(dirp);
   return 0;
}

void rfs_rewinddir(rfs_dir_t *dirp) {
   if (!dirp) {
      return;
   }
   if (dirp->real_dir != NULL) {
#undef rewinddir
      rewinddir(dirp->real_dir);
      return;
   }
   dirp->current_idx = 0;
}

long rfs_telldir(rfs_dir_t *dirp) {
   if (!dirp) {
      errno = EBADF;
      return -1;
   }
   if (dirp->real_dir != NULL) {
#undef telldir
      return telldir(dirp->real_dir);
   }
   return (long)dirp->current_idx;
}

void rfs_seekdir(rfs_dir_t *dirp, long loc) {
   if (!dirp || loc < 0) {
      return;
   }
   if (dirp->real_dir != NULL) {
#undef seekdir
      seekdir(dirp->real_dir, loc);
      return;
   }
   dirp->current_idx = (size_t)loc > dirp->count ? dirp->count : (size_t)loc;
}
