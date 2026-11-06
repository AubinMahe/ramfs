#include "ramfs_core.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_portable(path) _mkdir(path)
#else
#include <sys/stat.h>
#define mkdir_portable(path) mkdir(path, 0755)
#endif

/* =========================================================================
 * Tar format header (POSIX ustar) - private to archive module
 * ========================================================================= */

struct tar_header {
   char name[100];
   char mode[8];
   char uid[8];
   char gid[8];
   char size[12];
   char mtime[12];
   char chksum[8];
   char typeflag;
   char linkname[100];
   char magic[6];
   char version[2];
   char uname[32];
   char gname[32];
   char devmajor[8];
   char devminor[8];
   char prefix[155];
   char pad[12];
};

/* =========================================================================
 * Physical disk directory utilities
 * ========================================================================= */

static void disk_mkdir_p(const char *dir_path) {
   char tmp[PATH_MAX];
   char *p = NULL;
   size_t len;
   snprintf(tmp, sizeof(tmp), "%s", dir_path);
   len = strlen(tmp);
   if (len == 0) {
      return;
   }
   if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') {
      tmp[len - 1] = 0;
   }
   for (p = tmp + 1; *p; p++) {
      if (*p == '/' || *p == '\\') {
         char c = *p;
         *p = 0;
         mkdir_portable(tmp);
         *p = c;
      }
   }
   mkdir_portable(tmp);
}

static void make_tar_header(struct tar_header *h, const char *path, size_t size, int is_dir) {
   memset(h, 0, sizeof(*h));
   char formatted_path[PATH_MAX + 1];
   if (is_dir) {
      size_t plen = strlen(path);
      if (plen > 0 && path[plen - 1] != '/') {
         snprintf(formatted_path, sizeof(formatted_path), "%s/", path);
      } else {
         snprintf(formatted_path, sizeof(formatted_path), "%s", path);
      }
   } else {
      snprintf(formatted_path, sizeof(formatted_path), "%s", path);
   }
   const char *target_path = formatted_path;
   if (strlen(target_path) > 100) {
      const char *slash = strrchr(target_path, '/');
      if (!slash) {
         slash = strrchr(target_path, '\\');
      }
      if (slash && (size_t)(slash - target_path) < sizeof(h->prefix) && strlen(slash + 1) < sizeof(h->name)) {
         memcpy(h->prefix, target_path, (size_t)(slash - target_path));
         strncpy(h->name, slash + 1, sizeof(h->name) - 1);
      } else {
         strncpy(h->name, target_path, sizeof(h->name) - 1);
      }
   } else {
      strncpy(h->name, target_path, sizeof(h->name) - 1);
   }
   snprintf(h->mode, sizeof(h->mode), "%07o", is_dir ? 0755 : 0644);
   snprintf(h->uid, sizeof(h->uid), "%07o", 1000);
   snprintf(h->gid, sizeof(h->gid), "%07o", 1000);
   snprintf(h->size, sizeof(h->size), "%011lo", (unsigned long)(is_dir ? 0 : size));
   snprintf(h->mtime, sizeof(h->mtime), "%011lo", (unsigned long)time(NULL));
   h->typeflag = is_dir ? '5' : '0';
   memcpy(h->magic, "ustar ", 6);
   memcpy(h->version, " ", 2);
   strncpy(h->uname, "user", sizeof(h->uname) - 1);
   strncpy(h->gname, "group", sizeof(h->gname) - 1);

   memset(h->chksum, ' ', sizeof(h->chksum));
   unsigned sum = 0;
   const unsigned char *p = (const unsigned char *)h;
   for (size_t i = 0; i < sizeof(*h); i++) {
      sum += p[i];
   }
   snprintf(h->chksum, sizeof(h->chksum), "%06o", sum);
}

static const ram_catalog_t **collect_save_entries(const char *const *paths, size_t path_count, size_t *out_count) {
   if (!g_files || !g_table) {
      errno = ENODEV;
      return NULL;
   }

   const ram_catalog_t **entries = NULL;
   size_t n_entries = 0;

   if (paths != NULL) {
      size_t count = path_count;
      if (count == 0) {
         while (paths[count] != NULL) {
            count++;
         }
      }
      if (count > 0) {
         entries = malloc(count * sizeof(ram_catalog_t *));
         if (!entries) {
            errno = ENOMEM;
            return NULL;
         }
         for (size_t i = 0; i < count; i++) {
            char clean_path[PATH_MAX];
            if (rfs_normalize_path(paths[i], clean_path, sizeof(clean_path)) != 0) {
               free(entries);
               return NULL;
            }
            ram_catalog_t *e = search_in_catalog(clean_path, hash_path(clean_path));
            if (!e || !e->file) {
               free(entries);
               errno = ENOENT;
               return NULL;
            }
            entries[n_entries++] = e;
         }
      }
   } else {
      entries = malloc((g_files_capacity + 1) * sizeof(ram_catalog_t *));
      if (!entries) {
         errno = ENOMEM;
         return NULL;
      }
      for (size_t i = 0; i <= g_table_mask; i++) {
         const ram_catalog_t *e = g_table + i;
         if (e->path != NULL && e->file != NULL) {
            entries[n_entries++] = e;
         }
      }
   }
   *out_count = n_entries;
   return entries;
}

static int write_file_to_gz(gzFile gz, const ram_file_t *file) {
   size_t off = 0;
   while (off < file->size) {
      size_t chunk_idx = off >> RAMFS_CHUNK_SHIFT;
      size_t chunk_off = off & RAMFS_CHUNK_MASK;
      size_t chunk_avail = RAMFS_CHUNK_SIZE - chunk_off;
      size_t remaining = file->size - off;
      size_t to_write = remaining < chunk_avail ? remaining : chunk_avail;
      const unsigned char *chunk = rfs_file_get_chunk(file, chunk_idx);
      if (!chunk || gzwrite(gz, chunk + chunk_off, (unsigned)to_write) != (int)to_write) {
         return -1;
      }
      off += to_write;
   }
   return 0;
}

static int write_file_to_disk(FILE *f, const ram_file_t *file) {
   size_t off = 0;
   while (off < file->size) {
      size_t chunk_idx = off >> RAMFS_CHUNK_SHIFT;
      size_t chunk_off = off & RAMFS_CHUNK_MASK;
      size_t chunk_avail = RAMFS_CHUNK_SIZE - chunk_off;
      size_t remaining = file->size - off;
      size_t to_write = remaining < chunk_avail ? remaining : chunk_avail;
      const unsigned char *chunk = rfs_file_get_chunk(file, chunk_idx);
      if (!chunk || fwrite(chunk + chunk_off, 1, to_write, f) != to_write) {
         return -1;
      }
      off += to_write;
   }
   return 0;
}

/* =========================================================================
 * Tar archive and directory export
 * ========================================================================= */

int rfs_save_archive(const char *archive_path, const char *const *paths, size_t path_count) {
   if (!archive_path) {
      errno = EINVAL;
      return -1;
   }

   size_t n_entries = 0;
   const ram_catalog_t **entries = collect_save_entries(paths, path_count, &n_entries);
   if (!entries && errno != 0) {
      return -1;
   }

   gzFile gz = gzopen(archive_path, "wb9");
   if (!gz) {
      if (errno == 0) {
         errno = EIO;
      }
      free(entries);
      return -1;
   }
   char zeros[512] = {0};
   for (size_t i = 0; i < n_entries; i++) {
      const ram_catalog_t *entry = entries[i];
      const ram_file_t *file = entry->file;
      struct tar_header h;
      make_tar_header(&h, entry->path, file->size, file->is_dir);
      if (gzwrite(gz, &h, sizeof(h)) != (int)sizeof(h)) {
         if (errno == 0) {
            errno = EIO;
         }
         gzclose(gz);
         free(entries);
         return -1;
      }
      if (file->is_dir) {
         continue;
      }
      if (file->size > 0) {
         if (write_file_to_gz(gz, file) != 0) {
            if (errno == 0) {
               errno = EIO;
            }
            gzclose(gz);
            free(entries);
            return -1;
         }
      }
      size_t pad = (512 - (file->size % 512)) % 512;
      if (pad > 0) {
         if (gzwrite(gz, zeros, (unsigned)pad) != (int)pad) {
            if (errno == 0) {
               errno = EIO;
            }
            gzclose(gz);
            free(entries);
            return -1;
         }
      }
   }
   gzwrite(gz, zeros, 512);
   gzwrite(gz, zeros, 512);
   if (gzclose(gz) != Z_OK) {
      if (errno == 0) {
         errno = EIO;
      }
      free(entries);
      return -1;
   }
   free(entries);
   return 0;
}

int rfs_save_dir(const char *dest_dir, const char *const *paths, size_t path_count) {
   size_t n_entries = 0;
   const ram_catalog_t **entries = collect_save_entries(paths, path_count, &n_entries);
   if (!entries && errno != 0) {
      return -1;
   }

   if (dest_dir != NULL && strcmp(dest_dir, ".") != 0 && strcmp(dest_dir, "") != 0) {
      disk_mkdir_p(dest_dir);
   }
   for (size_t i = 0; i < n_entries; i++) {
      const ram_catalog_t *entry = entries[i];
      const ram_file_t *file = entry->file;
      char real_filepath[PATH_MAX];
      if (dest_dir != NULL && strcmp(dest_dir, ".") != 0 && strcmp(dest_dir, "") != 0) {
         snprintf(real_filepath, sizeof(real_filepath), "%s/%s", dest_dir, entry->path);
      } else {
         snprintf(real_filepath, sizeof(real_filepath), "%s", entry->path);
      }
      if (file->is_dir) {
         disk_mkdir_p(real_filepath);
         continue;
      }
      char parent_dir[PATH_MAX] = {0};
      const char *last_slash = strrchr(real_filepath, '/');
      const char *last_backslash = strrchr(real_filepath, '\\');
      const char *sep = (last_slash > last_backslash) ? last_slash : last_backslash;
      if (sep) {
         size_t sep_len = (size_t)(sep - real_filepath);
         if (sep_len >= sizeof(parent_dir)) {
            sep_len = sizeof(parent_dir) - 1;
         }
         memcpy(parent_dir, real_filepath, sep_len);
         disk_mkdir_p(parent_dir);
      }
      FILE *f = fopen(real_filepath, "wb");
      if (!f) {
         if (errno == 0) {
            errno = EIO;
         }
         free(entries);
         return -1;
      }
      if (file->size > 0) {
         if (write_file_to_disk(f, file) != 0) {
            if (errno == 0) {
               errno = EIO;
            }
            fclose(f);
            free(entries);
            return -1;
         }
      }
      if (fclose(f) != 0) {
         if (errno == 0) {
            errno = EIO;
         }
         free(entries);
         return -1;
      }
   }
   free(entries);
   return 0;
}

/* =========================================================================
 * Tar archive and directory import
 * ========================================================================= */

static void build_ram_target_path(char *dst, size_t dst_size, const char *prefix, const char *sub_rel) {
   if (prefix && prefix[0] != '\0') {
      size_t plen = strlen(prefix);
      if (prefix[plen - 1] == '/' || prefix[plen - 1] == '\\') {
         snprintf(dst, dst_size, "%s%s", prefix, sub_rel);
      } else {
         snprintf(dst, dst_size, "%s/%s", prefix, sub_rel);
      }
   } else {
      snprintf(dst, dst_size, "%s", sub_rel);
   }
}

static int import_regular_file(const char *full_disk_path, const char *ram_path) {
   FILE *f = fopen(full_disk_path, "rb");
   if (!f) {
      return -1;
   }
   rfs_handle_t fd = rfs_open(ram_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
   if (fd < 0) {
      int err = errno;
      fclose(f);
      errno = err;
      return -1;
   }
   char buf[4096];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
      if (rfs_write(fd, buf, n) < 0) {
         int err = errno;
         rfs_close(fd);
         fclose(f);
         errno = err;
         return -1;
      }
   }
   rfs_close(fd);
   fclose(f);
   return 0;
}

static int load_dir_recursive(const char *root_dir, const char *rel_sub, const char *prefix) {
   char dir_path[PATH_MAX * 2];
   if (rel_sub && rel_sub[0] != '\0') {
      snprintf(dir_path, sizeof(dir_path), "%s/%s", root_dir, rel_sub);
   } else {
      snprintf(dir_path, sizeof(dir_path), "%s", root_dir);
   }
   DIR *d = opendir(dir_path);
   if (!d) {
      return -1;
   }
   struct dirent *de;
   int count = 0;
   while ((de = readdir(d)) != NULL) {
      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
         continue;
      }
      char sub_rel[PATH_MAX];
      if (rel_sub && rel_sub[0] != '\0') {
         snprintf(sub_rel, sizeof(sub_rel), "%s/%s", rel_sub, de->d_name);
      } else {
         snprintf(sub_rel, sizeof(sub_rel), "%s", de->d_name);
      }
      char full_disk_path[PATH_MAX * 2];
      snprintf(full_disk_path, sizeof(full_disk_path), "%s/%s", root_dir, sub_rel);
      struct stat st;
      if (stat(full_disk_path, &st) != 0) {
         continue;
      }
      char ram_path[PATH_MAX * 2];
      build_ram_target_path(ram_path, sizeof(ram_path), prefix, sub_rel);
      if (S_ISDIR(st.st_mode)) {
         rfs_mkdir(ram_path);
         int sub_count = load_dir_recursive(root_dir, sub_rel, prefix);
         if (sub_count < 0) {
            closedir(d);
            return -1;
         }
         count += sub_count;
      } else if (S_ISREG(st.st_mode)) {
         if (import_regular_file(full_disk_path, ram_path) != 0) {
            closedir(d);
            return -1;
         }
         count++;
      }
   }
   closedir(d);
   return count;
}

static int unpack_tar_file(gzFile gz, const char *full_path, unsigned long file_size) {
   rfs_handle_t fd = rfs_open(full_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
   if (fd == RFS_INVALID_HANDLE) {
      return -1;
   }
   if (file_size > 0) {
      char buffer[4096];
      unsigned long remaining = file_size;
      while (remaining > 0) {
         unsigned to_read = remaining > sizeof(buffer) ? (unsigned)sizeof(buffer) : (unsigned)remaining;
         int bytes = gzread(gz, buffer, to_read);
         if (bytes <= 0) {
            int err = (errno != 0) ? errno : EIO;
            rfs_close(fd);
            errno = err;
            return -1;
         }
         rfs_write(fd, buffer, (size_t)bytes);
         remaining -= (unsigned long)bytes;
      }
   }
   rfs_close(fd);
   return 0;
}

static int load_from_archive(const char *archive_path, const char *prefix) {
   gzFile gz = gzopen(archive_path, "rb");
   if (!gz) {
      if (errno == 0) {
         errno = ENOENT;
      }
      return -1;
   }
   int files_loaded = 0;
   struct tar_header h;
   while (1) {
      int r = gzread(gz, &h, sizeof(h));
      if (r <= 0 || r != (int)sizeof(h)) {
         break;
      }
      int all_zero = 1;
      const unsigned char *hp = (const unsigned char *)&h;
      for (size_t i = 0; i < sizeof(h); i++) {
         if (hp[i] != 0) {
            all_zero = 0;
            break;
         }
      }
      if (all_zero) {
         break;
      }
      char item_name[256];
      if (h.prefix[0] != '\0') {
         snprintf(item_name, sizeof(item_name), "%.155s/%.100s", h.prefix, h.name);
      } else {
         snprintf(item_name, sizeof(item_name), "%.100s", h.name);
      }
      const char *clean_name = item_name;
      while (clean_name[0] == '.' && clean_name[1] == '/') {
         clean_name += 2;
      }
      while (clean_name[0] == '/') {
         clean_name++;
      }
      char full_path[PATH_MAX];
      if (prefix != NULL && prefix[0] != '\0') {
         size_t plen = strlen(prefix);
         if (prefix[plen - 1] == '/' || prefix[plen - 1] == '\\') {
            snprintf(full_path, sizeof(full_path), "%s%s", prefix, clean_name);
         } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", prefix, clean_name);
         }
      } else {
         snprintf(full_path, sizeof(full_path), "%s", clean_name);
      }
      unsigned long file_size = strtoul(h.size, NULL, 8);
      size_t pad = (512 - (file_size % 512)) % 512;
      if (h.typeflag == '5' || (h.typeflag == '\0' && clean_name[strlen(clean_name) - 1] == '/')) {
         char dir_clean[PATH_MAX];
         strncpy(dir_clean, full_path, sizeof(dir_clean) - 1);
         dir_clean[sizeof(dir_clean) - 1] = '\0';
         size_t dlen = strlen(dir_clean);
         if (dlen > 0 && dir_clean[dlen - 1] == '/') {
            dir_clean[dlen - 1] = '\0';
         }
         rfs_mkdir(dir_clean);
         continue;
      }
      if (unpack_tar_file(gz, full_path, file_size) != 0) {
         int err = errno;
         gzclose(gz);
         errno = err;
         return -1;
      }
      files_loaded++;
      if (pad > 0) {
         char dummy[512];
         gzread(gz, dummy, (unsigned)pad);
      }
   }
   gzclose(gz);
   return files_loaded;
}

int rfs_load(const char *archive_path, const char *src_dir, const char *prefix) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   if (archive_path != NULL) {
      return load_from_archive(archive_path, prefix);
   }
   const char *dir = src_dir ? src_dir : ".";
   int res = load_dir_recursive(dir, "", prefix);
   if (res < 0) {
      if (errno == 0) {
         errno = ENOENT;
      }
      return -1;
   }
   return res;
}

int rfs_load_dir(const char *dir_path, const char *prefix) { return rfs_load(NULL, dir_path, prefix); }
