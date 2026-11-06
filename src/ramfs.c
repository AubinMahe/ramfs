#include "ramfs.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC target("sse4.2")
#endif
#include <smmintrin.h>
#endif

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_portable(path) _mkdir(path)
typedef intptr_t ssize_t;
#else
#include <unistd.h>
#define mkdir_portable(path) mkdir(path, 0755)
#endif

#define RAMFS_INITIAL_CAPACITY 1024
#define MIN_TABLE_SIZE 8

typedef struct {
   unsigned char *data;
   size_t size;
   size_t capacity;
   int is_dir;
} ram_file_t;

/* Une entrée de la table de hachage. path == NULL signifie "case vide". */
typedef struct {
   char *path;
   ram_file_t *file;
   size_t hash;
} ram_catalog_t;

typedef struct {
   off_t offset;
   unsigned ref_count;
} ram_shared_offset_t;

typedef struct {
   ram_file_t *file;
   ram_shared_offset_t *shared_offset;
   int flags;
   int eof;
   int error;
   int is_tmpfile;
} ram_file_access_t;

/* Répertoire de travail virtuel */
static char g_cwd[PATH_MAX] = "/";

/* Pool de fichiers + pile des indices libres */
static ram_file_t *g_files = NULL;
static unsigned *g_free_stack = NULL;
static unsigned g_free_top = 0;
static unsigned g_files_capacity = 0;

/* Table de hachage : adressage ouvert, sondage linéaire, taille = puissance de 2,
   suppression par décalage arrière (pas de tombstones). */
static ram_catalog_t *g_table = NULL;
static size_t g_table_mask = 0;

static ram_file_access_t *g_access_table = NULL;
static unsigned g_access_capacity = 0;

/* Statistiques et jauges de crête */
static unsigned g_files_used = 0;
static unsigned g_peak_files = 0;
static unsigned g_access_used = 0;
static unsigned g_peak_access = 0;

int rfs_init(unsigned files_count, unsigned access_count) {
   rfs_destroy();
   size_t table_size = MIN_TABLE_SIZE;
   while (table_size < (size_t)files_count * 2) {
      table_size *= 2; /* facteur de charge <= 0,5 */
   }
   g_files = calloc(files_count ? files_count : 1, sizeof(ram_file_t));
   g_free_stack = malloc((files_count ? files_count : 1) * sizeof(unsigned));
   g_table = calloc(table_size, sizeof(ram_catalog_t));
   g_access_table = calloc(access_count ? access_count : 1, sizeof(ram_file_access_t));
   if ((g_files == NULL) || (g_free_stack == NULL) || (g_table == NULL) || (g_access_table == NULL)) {
      errno = ENOMEM;
      rfs_destroy();
      return -1;
   }
   g_files_capacity = files_count;
   g_table_mask = table_size - 1;
   g_access_capacity = access_count;
   g_files_used = 0;
   g_peak_files = 0;
   g_access_used = 0;
   g_peak_access = 0;
   snprintf(g_cwd, sizeof(g_cwd), "/");
   /* indices poussés en ordre décroissant : le 0 sort en premier */
   for (unsigned i = 0; i < files_count; i++) {
      g_free_stack[i] = files_count - 1 - i;
   }
   g_free_top = files_count;
   return 0;
}

void rfs_destroy(void) {
   if (g_table) {
      for (size_t i = 0; i <= g_table_mask; i++) {
         free(g_table[i].path);
      }
      free(g_table);
   }
   if (g_files) {
      for (unsigned i = 0; i < g_files_capacity; i++) {
         free(g_files[i].data);
      }
      free(g_files);
   }
   if (g_free_stack) {
      free(g_free_stack);
   }
   if (g_access_table) {
      for (unsigned i = 0; i < g_access_capacity; i++) {
         if (g_access_table[i].shared_offset) {
            if (--g_access_table[i].shared_offset->ref_count == 0) {
               free(g_access_table[i].shared_offset);
            }
            g_access_table[i].shared_offset = NULL;
         }
      }
      free(g_access_table);
   }
   snprintf(g_cwd, sizeof(g_cwd), "/");
   g_table = NULL;
   g_table_mask = 0;
   g_files = NULL;
   g_free_stack = NULL;
   g_free_top = 0;
   g_files_capacity = 0;
   g_access_table = NULL;
   g_access_capacity = 0;
   g_files_used = 0;
   g_peak_files = 0;
   g_access_used = 0;
   g_peak_access = 0;
}

static size_t hash_path(const char *s) {
#if defined(__x86_64__) || defined(_M_X64)
   uint64_t crc = 0xFFFFFFFF;
   size_t len = strlen(s);
   size_t chunks = len / 8;
   for (size_t i = 0; i < chunks; i++) {
      uint64_t v;
      memcpy(&v, s + i * 8, sizeof(v));
      crc = _mm_crc32_u64(crc, v);
   }
   for (size_t i = chunks * 8; i < len; i++) {
      crc = _mm_crc32_u8((uint32_t)crc, (unsigned char)s[i]);
   }
   return (size_t)crc;
#else
   uint64_t h = 1469598103934665603ULL;
   for (; *s; s++) {
      h ^= (unsigned char)*s;
      h *= 1099511628211ULL;
   }
   return (size_t)h;
#endif
}

static ram_catalog_t *search_in_catalog(const char *path, size_t hash) {
   if (!g_table) {
      return NULL;
   }
   size_t i = hash & g_table_mask;
   while (g_table[i].path != NULL) {
      if (g_table[i].hash == hash && strcmp(g_table[i].path, path) == 0) {
         return g_table + i;
      }
      i = (i + 1) & g_table_mask;
   }
   return NULL;
}

static ram_catalog_t *create_entry_in_catalog(const char *path, size_t hash) {
   if (g_free_top == 0) {
      fprintf(stderr, "Capacité du catalogue atteinte !\n");
      errno = ENOMEM;
      return NULL;
   }
   char *path_copy = strdup(path);
   unsigned char *data = calloc(1, RAMFS_INITIAL_CAPACITY);
   if (!path_copy || !data) {
      free(path_copy);
      free(data);
      errno = ENOMEM;
      return NULL;
   }
   ram_file_t *file = g_files + g_free_stack[--g_free_top];
   file->data = data;
   file->size = 0;
   file->capacity = RAMFS_INITIAL_CAPACITY;
   file->is_dir = 0;
   size_t i = hash & g_table_mask;
   while (g_table[i].path != NULL) {
      i = (i + 1) & g_table_mask;
   }
   g_table[i].path = path_copy;
   g_table[i].file = file;
   g_table[i].hash = hash;
   g_files_used++;
   if (g_files_used > g_peak_files) {
      g_peak_files = g_files_used;
   }
   return g_table + i;
}

static ram_catalog_t *create_dir_entry_in_catalog(const char *path, size_t hash) {
   if (g_free_top == 0) {
      fprintf(stderr, "Capacité du catalogue atteinte !\n");
      errno = ENOMEM;
      return NULL;
   }
   char *path_copy = strdup(path);
   if (!path_copy) {
      errno = ENOMEM;
      return NULL;
   }
   ram_file_t *file = g_files + g_free_stack[--g_free_top];
   file->data = NULL;
   file->size = 0;
   file->capacity = 0;
   file->is_dir = 1;
   size_t i = hash & g_table_mask;
   while (g_table[i].path != NULL) {
      i = (i + 1) & g_table_mask;
   }
   g_table[i].path = path_copy;
   g_table[i].file = file;
   g_table[i].hash = hash;
   g_files_used++;
   if (g_files_used > g_peak_files) {
      g_peak_files = g_files_used;
   }
   return g_table + i;
}

static int is_dir_prefix(const char *clean_path) {
   if (!g_table) {
      return 0;
   }
   size_t len = strlen(clean_path);
   for (size_t i = 0; i <= g_table_mask; i++) {
      const ram_catalog_t *e = g_table + i;
      if (e->path != NULL && e->file != NULL) {
         if (strncmp(e->path, clean_path, len) == 0 && e->path[len] == '/') {
            return 1;
         }
      }
   }
   return 0;
}

/* Retire l'entrée à l'index i en conservant la validité des chaînes de sondage. */
static void remove_from_catalog(size_t i) {
   size_t j = i;
   for (;;) {
      j = (j + 1) & g_table_mask;
      if (g_table[j].path == NULL) {
         break;
      }
      size_t k = g_table[j].hash & g_table_mask; /* position idéale de j */
      /* j peut être déplacé en i si k n'est pas cycliquement dans ]i, j] */
      int movable = (i <= j) ? (k <= i || k > j) : (k <= i && k > j);
      if (movable) {
         g_table[i] = g_table[j];
         i = j;
      }
   }
   memset(g_table + i, 0, sizeof(ram_catalog_t));
}

/* Renvoie l'accès pour un handle valide et ouvert, sinon NULL (errno = EBADF). */
static ram_file_access_t *get_access(rfs_handle_t fd) {
   if ((g_files == NULL) || (g_access_table == NULL)) {
      errno = ENODEV;
      return NULL;
   }
   if (fd < 0 || ((unsigned)fd) >= g_access_capacity || g_access_table[fd].file == NULL) {
      errno = EBADF;
      return NULL;
   }
   return g_access_table + fd;
}

int rfs_normalize_path(const char *src, char *dst, size_t dst_size) {
   if (!src || !dst || dst_size == 0) {
      errno = EINVAL;
      return -1;
   }
   if (src[0] == '\0') {
      dst[0] = '\0';
      return 0;
   }
   size_t len = strlen(src);
   if (len >= dst_size) {
      errno = ENAMETOOLONG;
      return -1;
   }

   size_t src_idx = 0;
   size_t dst_idx = 0;

   /* Lettre de lecteur Windows éventuelle (ex: "C:" ou "c:\") */
   if (isalpha((unsigned char)src[0]) && src[1] == ':') {
      dst[dst_idx++] = (char)toupper((unsigned char)src[0]);
      dst[dst_idx++] = ':';
      src_idx = 2;
   }

   int is_absolute = (src[src_idx] == '/' || src[src_idx] == '\\');
   if (is_absolute) {
      dst[dst_idx++] = '/';
      while (src[src_idx] == '/' || src[src_idx] == '\\') {
         src_idx++;
      }
   }

   size_t root_idx = dst_idx;

   while (src[src_idx] != '\0') {
      while (src[src_idx] == '/' || src[src_idx] == '\\') {
         src_idx++;
      }
      if (src[src_idx] == '\0') {
         break;
      }
      const char *comp_start = src + src_idx;
      while (src[src_idx] != '\0' && src[src_idx] != '/' && src[src_idx] != '\\') {
         src_idx++;
      }
      size_t comp_len = (size_t)(src + src_idx - comp_start);

      /* Composant "." ignoré */
      if (comp_len == 1 && comp_start[0] == '.') {
         continue;
      }

      /* Composant ".." */
      if (comp_len == 2 && comp_start[0] == '.' && comp_start[1] == '.') {
         if (dst_idx > root_idx) {
            size_t prev = dst_idx - 1;
            while (prev > root_idx && dst[prev] != '/') {
               prev--;
            }
            dst_idx = (dst[prev] == '/' && prev > root_idx) ? prev : root_idx;
         } else if (!is_absolute) {
            if (dst_idx > root_idx) {
               if (dst_idx + 1 >= dst_size) {
                  errno = ENAMETOOLONG;
                  return -1;
               }
               dst[dst_idx++] = '/';
            }
            if (dst_idx + 2 >= dst_size) {
               errno = ENAMETOOLONG;
               return -1;
            }
            dst[dst_idx++] = '.';
            dst[dst_idx++] = '.';
         }
         continue;
      }

      /* Composant normal */
      if (dst_idx > root_idx) {
         if (dst[dst_idx - 1] != '/') {
            if (dst_idx + 1 >= dst_size) {
               errno = ENAMETOOLONG;
               return -1;
            }
            dst[dst_idx++] = '/';
         }
      }
      if (dst_idx + comp_len >= dst_size) {
         errno = ENAMETOOLONG;
         return -1;
      }
      memcpy(dst + dst_idx, comp_start, comp_len);
      dst_idx += comp_len;
   }

   if (dst_idx == 0) {
      dst[dst_idx++] = '.';
   }
   dst[dst_idx] = '\0';
   return 0;
}

static int resolve_path(const char *src, char *dst, size_t dst_size) {
   if (!src || !dst || dst_size == 0) {
      errno = EINVAL;
      return -1;
   }
   int is_absolute = (src[0] == '/' || src[0] == '\\' || (isalpha((unsigned char)src[0]) && src[1] == ':'));
   if (!is_absolute && g_cwd[0] != '\0' && strcmp(g_cwd, "/") != 0 && strcmp(g_cwd, ".") != 0) {
      char combined[PATH_MAX * 2];
      const char *cwd_sub = (g_cwd[0] == '/') ? g_cwd + 1 : g_cwd;
      if (cwd_sub[0] != '\0') {
         int ret = snprintf(combined, sizeof(combined), "%s/%s", cwd_sub, src);
         if (ret < 0 || (size_t)ret >= sizeof(combined)) {
            errno = ENAMETOOLONG;
            return -1;
         }
         return rfs_normalize_path(combined, dst, dst_size);
      }
   }
   return rfs_normalize_path(src, dst, dst_size);
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
   if (clean_path[0] == '\0' || strcmp(clean_path, ".") == 0 || strcmp(clean_path, "/") == 0) {
      errno = EISDIR;
      return RFS_INVALID_HANDLE;
   }
   int access_mode = flags & O_ACCMODE;
   size_t hash = hash_path(clean_path);
   ram_catalog_t *entry = search_in_catalog(clean_path, hash);
   ram_file_t *file = NULL;
   if (entry) {
      file = entry->file;
      if (file->is_dir) {
         errno = EISDIR;
         return RFS_INVALID_HANDLE;
      }
      if ((flags & O_CREAT) && (flags & O_EXCL)) {
         errno = EEXIST;
         return RFS_INVALID_HANDLE;
      }
   } else {
      if (is_dir_prefix(clean_path)) {
         errno = EISDIR;
         return RFS_INVALID_HANDLE;
      }
      if (flags & O_CREAT) {
         entry = create_entry_in_catalog(clean_path, hash);
         if (entry == NULL) {
            return RFS_INVALID_HANDLE;
         }
         file = entry->file;
      } else {
         errno = ENOENT;
         return RFS_INVALID_HANDLE;
      }
   }
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
      fprintf(stderr, "Capacité d'accès atteinte !\n");
      errno = ENOMEM;
      return RFS_INVALID_HANDLE;
   }
   ram_shared_offset_t *so = malloc(sizeof(ram_shared_offset_t));
   if (!so) {
      errno = ENOMEM;
      return RFS_INVALID_HANDLE;
   }
   so->offset = (flags & O_APPEND) ? (off_t)file->size : 0;
   so->ref_count = 1;

   if (flags & O_TRUNC) {
      file->size = 0;
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
   return free_slot;
}

ssize_t rfs_read(rfs_handle_t fd, void *buf, size_t count) {
   ram_file_access_t *acc = get_access(fd);
   if (!acc || (acc->flags & O_ACCMODE) == O_WRONLY) {
      if (acc) {
         acc->error = 1;
         errno = EBADF;
      }
      return -1;
   }
   ram_file_t *file = acc->file;
   if (acc->shared_offset->offset >= (off_t)file->size) {
      if (count > 0) {
         acc->eof = 1;
      }
      return 0;
   }
   size_t available = file->size - (size_t)acc->shared_offset->offset;
   size_t to_read = count > available ? available : count;
   memcpy(buf, file->data + acc->shared_offset->offset, to_read);
   acc->shared_offset->offset += (off_t)to_read;
   if (count > available) {
      acc->eof = 1;
   }
   return (ssize_t)to_read;
}

ssize_t rfs_write(rfs_handle_t fd, const void *buf, size_t count) {
   ram_file_access_t *acc = get_access(fd);
   if (!acc || (acc->flags & O_ACCMODE) == O_RDONLY) {
      if (acc) {
         acc->error = 1;
         errno = EBADF;
      }
      return -1;
   }
   ram_file_t *file = acc->file;
   if (acc->flags & O_APPEND) {
      acc->shared_offset->offset = (off_t)file->size;
   }
   size_t required_size = (size_t)acc->shared_offset->offset + count;
   if (required_size > file->capacity) {
      size_t new_capacity = file->capacity == 0 ? RAMFS_INITIAL_CAPACITY : file->capacity * 2;
      while (new_capacity < required_size) {
         new_capacity *= 2;
      }
      unsigned char *new_data = realloc(file->data, new_capacity);
      if (!new_data) {
         acc->error = 1;
         errno = ENOMEM;
         return -1;
      }
      file->data = new_data;
      file->capacity = new_capacity;
   }
   if ((size_t)acc->shared_offset->offset > file->size) {
      /* écriture après un lseek au-delà de la fin : le trou est rempli de zéros */
      memset(file->data + file->size, 0, (size_t)acc->shared_offset->offset - file->size);
   }
   memcpy(file->data + acc->shared_offset->offset, buf, count);
   acc->shared_offset->offset += (off_t)count;
   if ((size_t)acc->shared_offset->offset > file->size) {
      file->size = (size_t)acc->shared_offset->offset;
   }
   return (ssize_t)count;
}

off_t rfs_lseek(rfs_handle_t fd, off_t offset, int whence) {
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      return -1;
   }
   off_t new_offset = acc->shared_offset->offset;
   switch (whence) {
   case SEEK_SET:
      new_offset = offset;
      break;
   case SEEK_CUR:
      new_offset += offset;
      break;
   case SEEK_END:
      new_offset = (off_t)acc->file->size + offset;
      break;
   default:
      errno = EINVAL;
      return -1;
   }
   if (new_offset < 0) {
      errno = EINVAL;
      return -1;
   }
   acc->shared_offset->offset = new_offset;
   acc->eof = 0;
   return acc->shared_offset->offset;
}

int rfs_close(rfs_handle_t fd) {
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      return -1;
   }
   if (acc->shared_offset) {
      if (--acc->shared_offset->ref_count == 0) {
         free(acc->shared_offset);
      }
      acc->shared_offset = NULL;
   }
   memset(acc, 0, sizeof(ram_file_access_t));
   if (g_access_used > 0) {
      g_access_used--;
   }
   return 0;
}

int rfs_unlink(const char *pathname) {
   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   if (clean_path[0] == '\0' || strcmp(clean_path, ".") == 0 || strcmp(clean_path, "/") == 0) {
      errno = EISDIR;
      return -1;
   }
   ram_catalog_t *entry = search_in_catalog(clean_path, hash_path(clean_path));
   if (entry == NULL) {
      if (is_dir_prefix(clean_path)) {
         errno = EISDIR;
         return -1;
      }
      errno = ENOENT;
      return -1;
   }
   ram_file_t *file = entry->file;
   if (file->is_dir) {
      errno = EISDIR;
      return -1;
   }
   for (unsigned i = 0; i < g_access_capacity; i++) {
      if (g_access_table[i].file == file) {
         errno = EBUSY;
         return -1;
      }
   }
   free(entry->path);
   free(file->data);
   unsigned index = (unsigned)(file - g_files);
   memset(file, 0, sizeof(ram_file_t));
   g_free_stack[g_free_top++] = index;
   remove_from_catalog((size_t)(entry - g_table));
   if (g_files_used > 0) {
      g_files_used--;
   }
   return 0;
}

int rfs_remove(const char *pathname) {
   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   ram_catalog_t *entry = search_in_catalog(clean_path, hash_path(clean_path));
   if ((entry && entry->file && entry->file->is_dir) || is_dir_prefix(clean_path)) {
      return rfs_rmdir(pathname);
   }
   return rfs_unlink(pathname);
}

static void fill_stat_buf(const ram_file_t *file, struct stat *buf) {
   memset(buf, 0, sizeof(struct stat));
   if (file && file->is_dir) {
      buf->st_mode = S_IFDIR | 0755;
      buf->st_size = 0;
      buf->st_nlink = 2;
   } else {
      buf->st_mode = S_IFREG | 0644;
      buf->st_size = file ? (off_t)file->size : 0;
      buf->st_nlink = 1;
   }
#if defined(__linux__) || defined(__unix__)
   buf->st_blksize = 4096;
   buf->st_blocks = (buf->st_size + 511) / 512;
#endif
   time_t now = time(NULL);
   buf->st_atime = now;
   buf->st_mtime = now;
   buf->st_ctime = now;
}

int rfs_stat(const char *pathname, struct stat *buf) {
   if (!pathname || !buf) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   ram_catalog_t *entry = search_in_catalog(clean_path, hash_path(clean_path));
   if (entry && entry->file) {
      fill_stat_buf(entry->file, buf);
      return 0;
   }
   if (clean_path[0] == '\0' || strcmp(clean_path, ".") == 0 || strcmp(clean_path, "/") == 0 || is_dir_prefix(clean_path)) {
      fill_stat_buf(NULL, buf);
      buf->st_mode = S_IFDIR | 0755;
      buf->st_nlink = 2;
      return 0;
   }
   errno = ENOENT;
   return -1;
}

int rfs_fstat(rfs_handle_t fd, struct stat *buf) {
   if (!buf) {
      errno = EINVAL;
      return -1;
   }
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      return -1;
   }
   fill_stat_buf(acc->file, buf);
   return 0;
}

int rfs_rename(const char *oldpath, const char *newpath) {
   if (!oldpath || !newpath) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   char clean_old[PATH_MAX];
   char clean_new[PATH_MAX];
   if (resolve_path(oldpath, clean_old, sizeof(clean_old)) != 0 || resolve_path(newpath, clean_new, sizeof(clean_new)) != 0) {
      return -1;
   }
   if (strcmp(clean_old, clean_new) == 0) {
      ram_catalog_t *e = search_in_catalog(clean_old, hash_path(clean_old));
      if (!e || !e->file) {
         errno = ENOENT;
         return -1;
      }
      return 0;
   }
   ram_catalog_t *old_entry = search_in_catalog(clean_old, hash_path(clean_old));
   if (!old_entry || !old_entry->file) {
      errno = ENOENT;
      return -1;
   }
   ram_file_t *old_file = old_entry->file;

   ram_catalog_t *new_entry = search_in_catalog(clean_new, hash_path(clean_new));
   if (new_entry && new_entry->file) {
      if (new_entry->file->is_dir != old_file->is_dir) {
         errno = old_file->is_dir ? ENOTDIR : EISDIR;
         return -1;
      }
      if (new_entry->file->is_dir && is_dir_prefix(clean_new)) {
         errno = ENOTEMPTY;
         return -1;
      }
      for (unsigned i = 0; i < g_access_capacity; i++) {
         if (g_access_table[i].file == new_entry->file) {
            errno = EBUSY;
            return -1;
         }
      }
   }

   char *new_path_copy = strdup(clean_new);
   if (!new_path_copy) {
      errno = ENOMEM;
      return -1;
   }

   if (new_entry && new_entry->file) {
      free(new_entry->path);
      free(new_entry->file->data);
      unsigned index = (unsigned)(new_entry->file - g_files);
      memset(new_entry->file, 0, sizeof(ram_file_t));
      g_free_stack[g_free_top++] = index;
      remove_from_catalog((size_t)(new_entry - g_table));
      if (g_files_used > 0) {
         g_files_used--;
      }
   }

   old_entry = search_in_catalog(clean_old, hash_path(clean_old));
   free(old_entry->path);
   remove_from_catalog((size_t)(old_entry - g_table));

   size_t hash = hash_path(clean_new);
   size_t i = hash & g_table_mask;
   while (g_table[i].path != NULL) {
      i = (i + 1) & g_table_mask;
   }
   g_table[i].path = new_path_copy;
   g_table[i].file = old_file;
   g_table[i].hash = hash;

   if (old_file->is_dir) {
      size_t old_len = strlen(clean_old);
      for (size_t idx = 0; idx <= g_table_mask; idx++) {
         ram_catalog_t *e = g_table + idx;
         if (e->path != NULL && e->file != NULL && e->file != old_file) {
            if (strncmp(e->path, clean_old, old_len) == 0 && e->path[old_len] == '/') {
               char child_new[PATH_MAX];
               snprintf(child_new, sizeof(child_new), "%s%s", clean_new, e->path + old_len);
               char *child_copy = strdup(child_new);
               if (child_copy) {
                  ram_file_t *cf = e->file;
                  free(e->path);
                  remove_from_catalog(idx);
                  size_t chash = hash_path(child_new);
                  size_t pos = chash & g_table_mask;
                  while (g_table[pos].path != NULL) {
                     pos = (pos + 1) & g_table_mask;
                  }
                  g_table[pos].path = child_copy;
                  g_table[pos].file = cf;
                  g_table[pos].hash = chash;
                  idx = (size_t)-1;
               }
            }
         }
      }
   }

   return 0;
}

int rfs_access(const char *pathname, int mode) {
   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   ram_catalog_t *entry = search_in_catalog(clean_path, hash_path(clean_path));
   int is_dir = 0;
   if (entry && entry->file) {
      is_dir = entry->file->is_dir;
   } else if (clean_path[0] == '\0' || strcmp(clean_path, ".") == 0 || strcmp(clean_path, "/") == 0 || is_dir_prefix(clean_path)) {
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
   va_list ap;
   va_start(ap, pathname);
   va_end(ap);

   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   if (clean_path[0] == '\0' || strcmp(clean_path, ".") == 0 || strcmp(clean_path, "/") == 0) {
      errno = EEXIST;
      return -1;
   }
   size_t hash = hash_path(clean_path);
   ram_catalog_t *entry = search_in_catalog(clean_path, hash);
   if (entry != NULL || is_dir_prefix(clean_path)) {
      errno = EEXIST;
      return -1;
   }
   const char *last_slash = strrchr(clean_path, '/');
   if (last_slash) {
      char parent[PATH_MAX];
      size_t plen = (size_t)(last_slash - clean_path);
      if (plen > 0) {
         memcpy(parent, clean_path, plen);
         parent[plen] = '\0';
         ram_catalog_t *pe = search_in_catalog(parent, hash_path(parent));
         if (pe) {
            if (!pe->file->is_dir) {
               errno = ENOTDIR;
               return -1;
            }
         } else if (!is_dir_prefix(parent)) {
            errno = ENOENT;
            return -1;
         }
      }
   }
   if (!create_dir_entry_in_catalog(clean_path, hash)) {
      return -1;
   }
   return 0;
}

int rfs_rmdir(const char *pathname) {
   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   if (clean_path[0] == '\0' || strcmp(clean_path, ".") == 0 || strcmp(clean_path, "/") == 0) {
      errno = EBUSY;
      return -1;
   }
   ram_catalog_t *entry = search_in_catalog(clean_path, hash_path(clean_path));
   if (!entry || !entry->file) {
      if (is_dir_prefix(clean_path)) {
         errno = ENOTEMPTY;
         return -1;
      }
      errno = ENOENT;
      return -1;
   }
   if (!entry->file->is_dir) {
      errno = ENOTDIR;
      return -1;
   }
   size_t len = strlen(clean_path);
   for (size_t i = 0; i <= g_table_mask; i++) {
      const ram_catalog_t *e = g_table + i;
      if (e->path != NULL && e->file != NULL && e != entry) {
         if (strncmp(e->path, clean_path, len) == 0 && e->path[len] == '/') {
            errno = ENOTEMPTY;
            return -1;
         }
      }
   }
   free(entry->path);
   unsigned index = (unsigned)(entry->file - g_files);
   memset(entry->file, 0, sizeof(ram_file_t));
   g_free_stack[g_free_top++] = index;
   remove_from_catalog((size_t)(entry - g_table));
   if (g_files_used > 0) {
      g_files_used--;
   }
   return 0;
}

int rfs_isatty(rfs_handle_t fd) {
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
#undef isatty
#ifdef _WIN32
      return _isatty(fd);
#else
      return isatty(fd);
#endif
   }
   errno = ENOTTY;
   return 0;
}

int rfs_get_stats(rfs_stats_t *stats) {
   if (!stats) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }
   stats->files_used = g_files_used;
   stats->peak_files = g_peak_files;
   stats->files_capacity = g_files_capacity;
   stats->access_used = g_access_used;
   stats->peak_access = g_peak_access;
   stats->access_capacity = g_access_capacity;
   return 0;
}

#ifndef RAMFS_VERSION
#define RAMFS_VERSION "unknown"
#endif

const char *rfs_version(void) { return RAMFS_VERSION; }

void rfs_dump_stats(FILE *stream) {
   FILE *out = stream ? stream : stderr;
   rfs_stats_t st;
   if (rfs_get_stats(&st) != 0) {
      fprintf(out, "ramfile: non initialisé (version %s)\n", rfs_version());
      return;
   }
   double files_pct = st.files_capacity ? ((double)st.peak_files * 100.0 / (double)st.files_capacity) : 0.0;
   double access_pct = st.access_capacity ? ((double)st.peak_access * 100.0 / (double)st.access_capacity) : 0.0;
   fprintf(out, "=== Statistiques ramfs (%s) ===\n", rfs_version());
   fprintf(out, "Fichiers : actuel %u, pic %u / %u (%.1f%%)\n", st.files_used, st.peak_files, st.files_capacity, files_pct);
   fprintf(out, "Accès    : actuel %u, pic %u / %u (%.1f%%)\n", st.access_used, st.peak_access, st.access_capacity, access_pct);
   fprintf(out, "============================\n");
}

#define STDIO_HANDLE_BIAS ((uintptr_t)0xBA5E0000)

static FILE *handle_to_file_ptr(rfs_handle_t handle) {
   if (handle == RFS_INVALID_HANDLE) {
      return NULL;
   }
   return (FILE *)(STDIO_HANDLE_BIAS + (uintptr_t)handle);
}

static rfs_handle_t file_ptr_to_handle(FILE *stream) {
   if (!stream) {
      return RFS_INVALID_HANDLE;
   }
   uintptr_t addr = (uintptr_t)stream;
   /* Seule la plage [BIAS, BIAS + access_count) désigne un faux FILE* de ramfile.
      Tout le reste (stdin, stdout, stderr, vrai FILE*) est rejeté. */
   if (addr < STDIO_HANDLE_BIAS || addr - STDIO_HANDLE_BIAS >= g_access_capacity) {
      return RFS_INVALID_HANDLE;
   }
   return (rfs_handle_t)(addr - STDIO_HANDLE_BIAS);
}

FILE *rfs_fopen(const char *pathname, const char *mode) {
   if (!pathname || !mode) {
      errno = EINVAL;
      return NULL;
   }
   int flags = 0;
   if (strchr(mode, 'r')) {
      flags = strchr(mode, '+') ? O_RDWR : O_RDONLY;
   } else if (strchr(mode, 'w')) {
      flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
   } else if (strchr(mode, 'a')) {
      flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
   } else {
      errno = EINVAL;
      return NULL;
   }
   rfs_handle_t handle = rfs_open(pathname, flags, 0644);
   return handle_to_file_ptr(handle);
}

size_t rfs_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE || size == 0 || nmemb == 0) {
      return 0;
   }
   ssize_t bytes_read = rfs_read(handle, ptr, size * nmemb);
   if (bytes_read <= 0) {
      return 0;
   }
   return (size_t)bytes_read / size;
}

size_t rfs_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE || size == 0 || nmemb == 0) {
      return 0;
   }
   ssize_t bytes_written = rfs_write(handle, ptr, size * nmemb);
   if (bytes_written <= 0) {
      return 0;
   }
   return (size_t)bytes_written / size;
}

int rfs_fseek(FILE *stream, long offset, int whence) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return -1;
   }
   off_t res = rfs_lseek(handle, (off_t)offset, whence);
   if (res == (off_t)-1) {
      return -1;
   }
   ram_file_access_t *acc = get_access(handle);
   if (acc) {
      acc->eof = 0;
   }
   return 0;
}

long rfs_ftell(FILE *stream) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return -1;
   }
   off_t current_offset = rfs_lseek(handle, 0, SEEK_CUR);
   return (long)current_offset;
}

void rfs_rewind(FILE *stream) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      return;
   }
   ram_file_access_t *acc = get_access(handle);
   if (acc && acc->shared_offset) {
      acc->shared_offset->offset = 0;
      acc->eof = 0;
      acc->error = 0;
   }
}

int rfs_fflush(FILE *stream) {
   if (!stream) {
      return 0;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (stream == stdout || stream == stderr || stream == stdin) {
#undef fflush
         return fflush(stream);
      }
      errno = EBADF;
      return EOF;
   }
   ram_file_access_t *acc = get_access(handle);
   if (!acc) {
      errno = EBADF;
      return EOF;
   }
   return 0;
}

int rfs_feof(FILE *stream) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      return 0;
   }
   ram_file_access_t *acc = get_access(handle);
   return acc ? acc->eof : 0;
}

int rfs_ferror(FILE *stream) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      return 0;
   }
   ram_file_access_t *acc = get_access(handle);
   return acc ? acc->error : 0;
}

void rfs_clearerr(FILE *stream) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      return;
   }
   ram_file_access_t *acc = get_access(handle);
   if (acc) {
      acc->eof = 0;
      acc->error = 0;
   }
}

int rfs_fileno(FILE *stream) {
   if (!stream) {
      errno = EBADF;
      return -1;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (stream == stdin || stream == stdout || stream == stderr) {
#undef fileno
         return fileno(stream);
      }
      errno = EBADF;
      return -1;
   }
   ram_file_access_t *acc = get_access(handle);
   if (!acc) {
      return -1;
   }
   return (int)handle;
}

int rfs_ungetc(int c, FILE *stream) {
   if (c == EOF) {
      return EOF;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return EOF;
   }
   ram_file_access_t *acc = get_access(handle);
   if (!acc || (acc->flags & O_ACCMODE) == O_WRONLY) {
      if (acc) {
         acc->error = 1;
         errno = EBADF;
      }
      return EOF;
   }
   if (acc->shared_offset->offset <= 0) {
      return EOF;
   }
   acc->shared_offset->offset--;
   acc->file->data[acc->shared_offset->offset] = (unsigned char)c;
   acc->eof = 0;
   return (int)(unsigned char)c;
}

int rfs_fgetc(FILE *stream) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return EOF;
   }
   ram_file_access_t *acc = get_access(handle);
   if (!acc || (acc->flags & O_ACCMODE) == O_WRONLY) {
      if (acc) {
         acc->error = 1;
         errno = EBADF;
      }
      return EOF;
   }
   ram_file_t *file = acc->file;
   if (acc->shared_offset->offset >= (off_t)file->size) {
      acc->eof = 1;
      return EOF;
   }
   unsigned char c = file->data[acc->shared_offset->offset++];
   return (int)c;
}

char *rfs_fgets(char *s, int size, FILE *stream) {
   if (!s || size <= 0) {
      errno = EINVAL;
      return NULL;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return NULL;
   }
   ram_file_access_t *acc = get_access(handle);
   if (!acc || (acc->flags & O_ACCMODE) == O_WRONLY) {
      if (acc) {
         acc->error = 1;
         errno = EBADF;
      }
      return NULL;
   }
   ram_file_t *file = acc->file;
   if (acc->shared_offset->offset >= (off_t)file->size) {
      acc->eof = 1;
      return NULL;
   }
   int i = 0;
   while (i < size - 1 && acc->shared_offset->offset < (off_t)file->size) {
      char c = (char)file->data[acc->shared_offset->offset++];
      s[i++] = c;
      if (c == '\n') {
         break;
      }
   }
   s[i] = '\0';
   return s;
}

int rfs_fputc(int c, FILE *stream) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return EOF;
   }
   unsigned char uc = (unsigned char)c;
   ssize_t written = rfs_write(handle, &uc, 1);
   if (written != 1) {
      ram_file_access_t *acc = get_access(handle);
      if (acc) {
         acc->error = 1;
      }
      return EOF;
   }
   return (int)uc;
}

int rfs_fputs(const char *s, FILE *stream) {
   if (!s) {
      errno = EINVAL;
      return EOF;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return EOF;
   }
   size_t len = strlen(s);
   if (len == 0) {
      return 0;
   }
   ssize_t written = rfs_write(handle, s, len);
   if (written < 0 || (size_t)written != len) {
      ram_file_access_t *acc = get_access(handle);
      if (acc) {
         acc->error = 1;
      }
      return EOF;
   }
   return (int)written;
}

int rfs_vfprintf(FILE *stream, const char *format, va_list ap) {
   if (!stream || !format) {
      errno = EINVAL;
      return -1;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return -1;
   }
   va_list ap_copy;
   va_copy(ap_copy, ap);
   int len = vsnprintf(NULL, 0, format, ap_copy);
   va_end(ap_copy);
   if (len < 0) {
      return -1;
   }
   if (len == 0) {
      return 0;
   }
   char *buffer = malloc((size_t)len + 1);
   if (!buffer) {
      errno = ENOMEM;
      return -1;
   }
   va_copy(ap_copy, ap);
   vsnprintf(buffer, (size_t)len + 1, format, ap_copy);
   va_end(ap_copy);
   ssize_t written = rfs_write(handle, buffer, (size_t)len);
   free(buffer);
   return (written < 0) ? -1 : (int)written;
}

int rfs_fprintf(FILE *stream, const char *format, ...) {
   va_list args;
   va_start(args, format);
   int ret = rfs_vfprintf(stream, format, args);
   va_end(args);
   return ret;
}

int rfs_fscanf(FILE *stream, const char *format, ...) {
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE || !format) {
      errno = EBADF;
      return -1;
   }
   ram_file_access_t *acc = get_access(handle);
   if (!acc) {
      return -1;
   }
   if ((acc->flags & O_ACCMODE) == O_WRONLY) {
      errno = EBADF;
      return -1;
   }
   ram_file_t *file = acc->file;
   if (acc->shared_offset->offset >= (off_t)file->size) {
      acc->eof = 1;
      return EOF;
   }
   /* vsscanf exige une chaîne terminée par '\0' : on travaille sur une copie. */
   size_t remaining = file->size - (size_t)acc->shared_offset->offset;
   char *text = malloc(remaining + 1);
   if (!text) {
      acc->error = 1;
      errno = ENOMEM;
      return -1;
   }
   memcpy(text, file->data + acc->shared_offset->offset, remaining);
   text[remaining] = '\0';
   size_t fmt_len = strlen(format);
   va_list args;
   va_start(args, format);
   int fields_read = vsscanf(text, format, args);
   va_end(args);
   if (fields_read == EOF) {
      acc->eof = 1;
      free(text);
      return EOF;
   }
   char *suppress_format = malloc(fmt_len * 2 + 16);
   if (suppress_format) {
      size_t j = 0;
      for (size_t i = 0; i < fmt_len; i++) {
         suppress_format[j++] = format[i];
         if (format[i] == '%' && i + 1 < fmt_len) {
            if (format[i + 1] == '%') {
               suppress_format[j++] = format[++i]; /* "%%" copié tel quel */
            } else {
               suppress_format[j++] = '*';
            }
         }
      }
      suppress_format[j] = '\0';
      strcat(suppress_format, "%n");
      int bytes_consumed = 0;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
      sscanf(text, suppress_format, &bytes_consumed);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
      if (bytes_consumed > 0) {
         acc->shared_offset->offset += bytes_consumed;
      }
      free(suppress_format);
   }
   free(text);
   return fields_read;
}

int rfs_fclose(FILE *stream) {
   if (!stream) {
      errno = EBADF;
      return -1;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return -1;
   }
   ram_file_access_t *acc = get_access(handle);
   if (!acc) {
      return -1;
   }
   int is_tmp = acc->is_tmpfile;
   ram_file_t *tmp_file = is_tmp ? acc->file : NULL;
   int res = rfs_close(handle);
   if (res == 0 && is_tmp && tmp_file) {
      for (size_t i = 0; i <= g_table_mask; i++) {
         if (g_table[i].file == tmp_file && g_table[i].path != NULL) {
            char *tmp_path = strdup(g_table[i].path);
            if (tmp_path) {
               rfs_unlink(tmp_path);
               free(tmp_path);
            }
            break;
         }
      }
   }
   return res;
}

int rfs_dup(rfs_handle_t oldfd) {
   ram_file_access_t *old_acc = get_access(oldfd);
   if (!old_acc) {
      return -1;
   }
   rfs_handle_t free_slot = RFS_INVALID_HANDLE;
   for (unsigned i = 0; i < g_access_capacity; i++) {
      if (g_access_table[i].file == NULL) {
         free_slot = (rfs_handle_t)i;
         break;
      }
   }
   if (free_slot == RFS_INVALID_HANDLE) {
      errno = EMFILE;
      return -1;
   }
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
   return free_slot;
}

int rfs_dup2(rfs_handle_t oldfd, rfs_handle_t newfd) {
   ram_file_access_t *old_acc = get_access(oldfd);
   if (!old_acc) {
      return -1;
   }
   if (newfd < 0 || (unsigned)newfd >= g_access_capacity) {
      errno = EBADF;
      return -1;
   }
   if (oldfd == newfd) {
      return newfd;
   }
   if (g_access_table[newfd].file != NULL) {
      rfs_close(newfd);
   }
   ram_file_access_t *new_acc = g_access_table + newfd;
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
   return newfd;
}

int rfs_ftruncate(rfs_handle_t fd, off_t length) {
   if (length < 0) {
      errno = EINVAL;
      return -1;
   }
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      return -1;
   }
   if ((acc->flags & O_ACCMODE) == O_RDONLY) {
      errno = EINVAL;
      return -1;
   }
   ram_file_t *file = acc->file;
   size_t new_len = (size_t)length;
   if (new_len > file->capacity) {
      size_t new_cap = file->capacity == 0 ? RAMFS_INITIAL_CAPACITY : file->capacity * 2;
      while (new_cap < new_len) {
         new_cap *= 2;
      }
      unsigned char *new_data = realloc(file->data, new_cap);
      if (!new_data) {
         acc->error = 1;
         errno = ENOMEM;
         return -1;
      }
      file->data = new_data;
      file->capacity = new_cap;
   }
   if (new_len > file->size) {
      memset(file->data + file->size, 0, new_len - file->size);
   }
   file->size = new_len;
   return 0;
}

int rfs_truncate(const char *pathname, off_t length) {
   if (!pathname || length < 0) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return -1;
   }
   ram_catalog_t *entry = search_in_catalog(clean_path, hash_path(clean_path));
   if (!entry || !entry->file) {
      if (is_dir_prefix(clean_path)) {
         errno = EISDIR;
         return -1;
      }
      errno = ENOENT;
      return -1;
   }
   ram_file_t *file = entry->file;
   if (file->is_dir) {
      errno = EISDIR;
      return -1;
   }
   size_t new_len = (size_t)length;
   if (new_len > file->capacity) {
      size_t new_cap = file->capacity == 0 ? RAMFS_INITIAL_CAPACITY : file->capacity * 2;
      while (new_cap < new_len) {
         new_cap *= 2;
      }
      unsigned char *new_data = realloc(file->data, new_cap);
      if (!new_data) {
         errno = ENOMEM;
         return -1;
      }
      file->data = new_data;
      file->capacity = new_cap;
   }
   if (new_len > file->size) {
      memset(file->data + file->size, 0, new_len - file->size);
   }
   file->size = new_len;
   return 0;
}

int rfs_chdir(const char *path) {
   if (!path) {
      errno = EINVAL;
      return -1;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   char target[PATH_MAX];
   if (resolve_path(path, target, sizeof(target)) != 0) {
      return -1;
   }
   if (target[0] == '\0' || strcmp(target, "/") == 0 || strcmp(target, ".") == 0) {
      snprintf(g_cwd, sizeof(g_cwd), "/");
      return 0;
   }
   ram_catalog_t *entry = search_in_catalog(target, hash_path(target));
   if (entry) {
      if (!entry->file->is_dir) {
         errno = ENOTDIR;
         return -1;
      }
   } else if (!is_dir_prefix(target)) {
      errno = ENOENT;
      return -1;
   }
   size_t tlen = strlen(target);
   if (target[0] != '/') {
      if (tlen + 2 > sizeof(g_cwd)) {
         errno = ENAMETOOLONG;
         return -1;
      }
      g_cwd[0] = '/';
      memcpy(g_cwd + 1, target, tlen + 1);
   } else {
      if (tlen + 1 > sizeof(g_cwd)) {
         errno = ENAMETOOLONG;
         return -1;
      }
      memcpy(g_cwd, target, tlen + 1);
   }
   return 0;
}

char *rfs_getcwd(char *buf, size_t size) {
   if (!g_files || !g_table) {
      errno = ENODEV;
      return NULL;
   }
   size_t len = strlen(g_cwd);
   if (buf == NULL) {
      size_t alloc_sz = (size > len + 1) ? size : (len + 1);
      buf = malloc(alloc_sz);
      if (!buf) {
         errno = ENOMEM;
         return NULL;
      }
      memcpy(buf, g_cwd, len + 1);
      return buf;
   }
   if (size == 0) {
      errno = EINVAL;
      return NULL;
   }
   if (len + 1 > size) {
      errno = ERANGE;
      return NULL;
   }
   memcpy(buf, g_cwd, len + 1);
   return buf;
}

struct rfs_dir {
   struct rfs_dirent *entries;
   size_t count;
   size_t current_idx;
   struct rfs_dirent current;
};

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

rfs_dir_t *rfs_opendir(const char *name) {
   if (!name) {
      errno = EINVAL;
      return NULL;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return NULL;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(name, clean_path, sizeof(clean_path)) != 0) {
      return NULL;
   }

   int is_root = (clean_path[0] == '\0' || strcmp(clean_path, ".") == 0 || strcmp(clean_path, "/") == 0);
   if (!is_root) {
      ram_catalog_t *entry = search_in_catalog(clean_path, hash_path(clean_path));
      if (entry) {
         if (!entry->file->is_dir) {
            errno = ENOTDIR;
            return NULL;
         }
      } else if (!is_dir_prefix(clean_path)) {
         errno = ENOENT;
         return NULL;
      }
   }

   rfs_dir_t *dir = calloc(1, sizeof(rfs_dir_t));
   if (!dir) {
      errno = ENOMEM;
      return NULL;
   }

   size_t cap = 0;
   if (add_dirent(&dir->entries, &dir->count, &cap, 1, DT_DIR, ".") != 0 ||
       add_dirent(&dir->entries, &dir->count, &cap, 2, DT_DIR, "..") != 0) {
      free(dir->entries);
      free(dir);
      errno = ENOMEM;
      return NULL;
   }

   char prefix[PATH_MAX + 2];
   size_t prefix_len = 0;
   if (!is_root) {
      size_t clen = strlen(clean_path);
      if (clen + 2 > sizeof(prefix)) {
         errno = ENAMETOOLONG;
         return NULL;
      }
      memcpy(prefix, clean_path, clen);
      prefix[clen] = '/';
      prefix[clen + 1] = '\0';
      prefix_len = clen + 1;
   }

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
      } else {
         if (strncmp(e->path, prefix, prefix_len) == 0) {
            sub = e->path + prefix_len;
         }
      }

      if (sub && sub[0] != '\0') {
         const char *slash = strchr(sub, '/');
         if (!slash) {
            unsigned char type = e->file->is_dir ? DT_DIR : DT_REG;
            ino_t ino = (ino_t)((size_t)(e->file - g_files) + 10);
            if (add_dirent(&dir->entries, &dir->count, &cap, ino, type, sub) != 0) {
               free(dir->entries);
               free(dir);
               errno = ENOMEM;
               return NULL;
            }
         } else {
            char dir_name[256];
            size_t comp_len = (size_t)(slash - sub);
            if (comp_len >= sizeof(dir_name)) {
               comp_len = sizeof(dir_name) - 1;
            }
            memcpy(dir_name, sub, comp_len);
            dir_name[comp_len] = '\0';
            if (add_dirent(&dir->entries, &dir->count, &cap, 3, DT_DIR, dir_name) != 0) {
               free(dir->entries);
               free(dir);
               errno = ENOMEM;
               return NULL;
            }
         }
      }
   }

   return dir;
}

struct rfs_dirent *rfs_readdir(rfs_dir_t *dirp) {
   if (!dirp || !g_files || !g_table) {
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
   free(dirp->entries);
   free(dirp);
   return 0;
}

void rfs_rewinddir(rfs_dir_t *dirp) {
   if (dirp) {
      dirp->current_idx = 0;
   }
}

long rfs_telldir(rfs_dir_t *dirp) {
   if (!dirp) {
      errno = EBADF;
      return -1;
   }
   return (long)dirp->current_idx;
}

void rfs_seekdir(rfs_dir_t *dirp, long loc) {
   if (dirp && loc >= 0) {
      dirp->current_idx = (size_t)loc > dirp->count ? dirp->count : (size_t)loc;
   }
}

static unsigned g_tmpfile_counter = 0;

FILE *rfs_tmpfile(void) {
   char tmp_name[64];
   snprintf(tmp_name, sizeof(tmp_name), ".ramfs_tmp_%u_%u", (unsigned)time(NULL), ++g_tmpfile_counter);
   FILE *fp = rfs_fopen(tmp_name, "w+b");
   if (fp) {
      rfs_handle_t h = file_ptr_to_handle(fp);
      ram_file_access_t *acc = get_access(h);
      if (acc) {
         acc->is_tmpfile = 1;
      }
   }
   return fp;
}

ssize_t rfs_getdelim(char **lineptr, size_t *n, int delimiter, FILE *stream) {
   if (!lineptr || !n || !stream) {
      errno = EINVAL;
      return -1;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      errno = EBADF;
      return -1;
   }
   ram_file_access_t *acc = get_access(handle);
   if (!acc || (acc->flags & O_ACCMODE) == O_WRONLY) {
      if (acc) {
         acc->error = 1;
         errno = EBADF;
      }
      return -1;
   }
   ram_file_t *file = acc->file;
   off_t cur_off = acc->shared_offset->offset;
   if (cur_off >= (off_t)file->size) {
      acc->eof = 1;
      return -1;
   }

   if (*lineptr == NULL || *n == 0) {
      *n = 128;
      *lineptr = malloc(*n);
      if (!*lineptr) {
         acc->error = 1;
         errno = ENOMEM;
         return -1;
      }
   }

   size_t read_bytes = 0;
   while (cur_off < (off_t)file->size) {
      int c = file->data[cur_off++];
      if (read_bytes + 2 > *n) {
         size_t new_n = *n * 2;
         char *new_ptr = realloc(*lineptr, new_n);
         if (!new_ptr) {
            acc->error = 1;
            errno = ENOMEM;
            return -1;
         }
         *lineptr = new_ptr;
         *n = new_n;
      }
      (*lineptr)[read_bytes++] = (char)c;
      if (c == delimiter) {
         break;
      }
   }
   acc->shared_offset->offset = cur_off;
   if (cur_off >= (off_t)file->size && read_bytes == 0) {
      acc->eof = 1;
      return -1;
   }
   (*lineptr)[read_bytes] = '\0';
   return (ssize_t)read_bytes;
}

ssize_t rfs_getline(char **lineptr, size_t *n, FILE *stream) { return rfs_getdelim(lineptr, n, '\n', stream); }

static void utils_mkdir_p(const char *dir_path) {
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
      if (file->size > 0 && file->data != NULL) {
         if (gzwrite(gz, file->data, (unsigned)file->size) != (int)file->size) {
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
      utils_mkdir_p(dest_dir);
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
         utils_mkdir_p(real_filepath);
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
         utils_mkdir_p(parent_dir);
      }
      FILE *f = fopen(real_filepath, "wb");
      if (!f) {
         if (errno == 0) {
            errno = EIO;
         }
         free(entries);
         return -1;
      }
      if (file->size > 0 && file->data != NULL) {
         if (fwrite(file->data, 1, file->size, f) != file->size) {
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
      if (S_ISDIR(st.st_mode)) {
         char ram_dir[PATH_MAX * 2];
         if (prefix && prefix[0] != '\0') {
            size_t plen = strlen(prefix);
            if (prefix[plen - 1] == '/' || prefix[plen - 1] == '\\') {
               snprintf(ram_dir, sizeof(ram_dir), "%s%s", prefix, sub_rel);
            } else {
               snprintf(ram_dir, sizeof(ram_dir), "%s/%s", prefix, sub_rel);
            }
         } else {
            snprintf(ram_dir, sizeof(ram_dir), "%s", sub_rel);
         }
         rfs_mkdir(ram_dir);
         int sub_count = load_dir_recursive(root_dir, sub_rel, prefix);
         if (sub_count < 0) {
            closedir(d);
            return -1;
         }
         count += sub_count;
      } else if (S_ISREG(st.st_mode)) {
         char ram_path[PATH_MAX * 2];
         if (prefix && prefix[0] != '\0') {
            size_t plen = strlen(prefix);
            if (prefix[plen - 1] == '/' || prefix[plen - 1] == '\\') {
               snprintf(ram_path, sizeof(ram_path), "%s%s", prefix, sub_rel);
            } else {
               snprintf(ram_path, sizeof(ram_path), "%s/%s", prefix, sub_rel);
            }
         } else {
            snprintf(ram_path, sizeof(ram_path), "%s", sub_rel);
         }
         FILE *f = fopen(full_disk_path, "rb");
         if (!f) {
            closedir(d);
            return -1;
         }
         rfs_handle_t fd = rfs_open(ram_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
         if (fd < 0) {
            int err = errno;
            fclose(f);
            closedir(d);
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
               closedir(d);
               errno = err;
               return -1;
            }
         }
         rfs_close(fd);
         fclose(f);
         count++;
      }
   }
   closedir(d);
   return count;
}

int rfs_load(const char *archive_path, const char *src_dir, const char *prefix) {
   if (!g_files || !g_access_table) {
      errno = ENODEV;
      return -1;
   }

   if (archive_path != NULL) {
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
         rfs_handle_t fd = rfs_open(full_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
         if (fd == RFS_INVALID_HANDLE) {
            int err = errno;
            gzclose(gz);
            errno = err;
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
                  gzclose(gz);
                  errno = err;
                  return -1;
               }
               rfs_write(fd, buffer, (size_t)bytes);
               remaining -= (unsigned long)bytes;
            }
         }
         rfs_close(fd);
         files_loaded++;
         if (pad > 0) {
            char dummy[512];
            gzread(gz, dummy, (unsigned)pad);
         }
      }
      gzclose(gz);
      return files_loaded;
   } else {
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
}

int rfs_load_dir(const char *dir_path, const char *prefix) { return rfs_load(NULL, dir_path, prefix); }
