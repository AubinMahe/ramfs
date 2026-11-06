#include "ramfs_core.h"
#include "ramfs_vfs.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC target("sse4.2")
#endif
#include <smmintrin.h>
#endif

/* =========================================================================
 * Variables globales du noyau (Core)
 * ========================================================================= */

/* Pool de fichiers + pile des indices libres */
ram_file_t *g_files = NULL;
unsigned *g_free_stack = NULL;
unsigned g_free_top = 0;
unsigned g_files_capacity = 0;

/* Table de hachage : adressage ouvert, sondage linéaire, taille = puissance de 2,
   suppression par décalage arrière (pas de tombstones). */
ram_catalog_t *g_table = NULL;
size_t g_table_mask = 0;

/* Table des descripteurs d'accès ouverts */
ram_file_access_t *g_access_table = NULL;
unsigned g_access_capacity = 0;

/* Statistiques et jauges de crête */
unsigned g_files_used = 0;
unsigned g_peak_files = 0;
unsigned g_access_used = 0;
unsigned g_peak_access = 0;

/* Suivi de la profondeur d'arborescence (nombre maximal de '/') pour accélérer is_dir_prefix */
static unsigned g_max_depth = 0;
static unsigned g_depth_counts[64];

/* Pool et Slabs de blocs mémoires fixes (Chunked & Slab Storage) */
static unsigned char **g_chunk_pool = NULL;
static size_t g_chunk_pool_top = 0;
static size_t g_chunk_pool_capacity = 0;

static unsigned char **g_slab_list = NULL;
static size_t g_slab_count = 0;
static size_t g_slab_capacity = 0;

static void release_chunk(unsigned char *chunk) {
   if (!chunk) {
      return;
   }
   if (g_chunk_pool_top >= g_chunk_pool_capacity) {
      size_t new_cap = g_chunk_pool_capacity ? g_chunk_pool_capacity * 2 : 64;
      unsigned char **new_pool = realloc(g_chunk_pool, new_cap * sizeof(unsigned char *));
      if (!new_pool) {
         return;
      }
      g_chunk_pool = new_pool;
      g_chunk_pool_capacity = new_cap;
   }
   g_chunk_pool[g_chunk_pool_top++] = chunk;
}

static int allocate_slab(void) {
   if (g_slab_count >= g_slab_capacity) {
      size_t new_cap = g_slab_capacity ? g_slab_capacity * 2 : 16;
      unsigned char **new_slabs = realloc(g_slab_list, new_cap * sizeof(unsigned char *));
      if (!new_slabs) {
         return -1;
      }
      g_slab_list = new_slabs;
      g_slab_capacity = new_cap;
   }
   unsigned char *slab = malloc(RAMFS_SLAB_CHUNKS * RAMFS_CHUNK_SIZE);
   if (!slab) {
      return -1;
   }
   g_slab_list[g_slab_count++] = slab;
   for (size_t i = 0; i < RAMFS_SLAB_CHUNKS; i++) {
      release_chunk(slab + i * RAMFS_CHUNK_SIZE);
   }
   return 0;
}

static unsigned char *allocate_chunk(void) {
   if (g_chunk_pool_top == 0) {
      if (allocate_slab() != 0) {
         return malloc(RAMFS_CHUNK_SIZE);
      }
   }
   return g_chunk_pool[--g_chunk_pool_top];
}

static unsigned count_slashes(const char *s) {
   unsigned n = 0;
   while (*s) {
      if (*s == '/') {
         n++;
      }
      s++;
   }
   return n;
}

void catalog_depth_add(const char *path) {
   unsigned d = count_slashes(path);
   if (d < 64) {
      g_depth_counts[d]++;
   }
   if (d > g_max_depth) {
      g_max_depth = d;
   }
}

static void catalog_depth_remove(const char *path) {
   unsigned d = count_slashes(path);
   if (d < 64 && g_depth_counts[d] > 0) {
      g_depth_counts[d]--;
   }
   while (g_max_depth > 0 && g_depth_counts[g_max_depth] == 0) {
      g_max_depth--;
   }
}

/* =========================================================================
 * Fonctions de hachage et de gestion du catalogue
 * ========================================================================= */

size_t hash_path(const char *s) {
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

ram_catalog_t *search_in_catalog(const char *path, size_t hash) {
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

static void rebase_files_pointers(ptrdiff_t diff) {
   if (diff == 0) {
      return;
   }
   for (size_t i = 0; i <= g_table_mask; i++) {
      if (g_table[i].path != NULL && g_table[i].file != NULL) {
         g_table[i].file = (ram_file_t *)((char *)g_table[i].file + diff);
      }
   }
   for (unsigned i = 0; i < g_access_capacity; i++) {
      if (g_access_table[i].file != NULL) {
         g_access_table[i].file = (ram_file_t *)((char *)g_access_table[i].file + diff);
      }
   }
}

static int expand_files_capacity(void) {
   unsigned old_cap = g_files_capacity;
   unsigned new_cap = old_cap ? old_cap * 2 : 32;
   uintptr_t old_addr = (uintptr_t)g_files;
   ram_file_t *new_files = realloc(g_files, new_cap * sizeof(ram_file_t));
   if (!new_files) {
      errno = ENOMEM;
      return -1;
   }
   ptrdiff_t diff = (ptrdiff_t)((uintptr_t)new_files - old_addr);
   unsigned *new_stack = realloc(g_free_stack, new_cap * sizeof(unsigned));
   if (!new_stack) {
      g_files = new_files;
      rebase_files_pointers(diff);
      errno = ENOMEM;
      return -1;
   }
   memset(new_files + old_cap, 0, (new_cap - old_cap) * sizeof(ram_file_t));
   rebase_files_pointers(diff);
   g_files = new_files;
   g_free_stack = new_stack;
   for (unsigned i = new_cap; i > old_cap; i--) {
      g_free_stack[g_free_top++] = i - 1;
   }
   g_files_capacity = new_cap;
   return 0;
}

static int resize_catalog_table(void) {
   size_t old_table_size = g_table_mask + 1;
   size_t new_table_size = old_table_size * 2;
   ram_catalog_t *new_table = calloc(new_table_size, sizeof(ram_catalog_t));
   if (!new_table) {
      errno = ENOMEM;
      return -1;
   }
   size_t new_mask = new_table_size - 1;
   for (size_t i = 0; i < old_table_size; i++) {
      if (g_table[i].path != NULL) {
         size_t j = g_table[i].hash & new_mask;
         while (new_table[j].path != NULL) {
            j = (j + 1) & new_mask;
         }
         new_table[j] = g_table[i];
      }
   }
   free(g_table);
   g_table = new_table;
   g_table_mask = new_mask;
   return 0;
}

int expand_access_table(void) {
   unsigned old_cap = g_access_capacity;
   unsigned new_cap = old_cap ? old_cap * 2 : 16;
   ram_file_access_t *new_table = realloc(g_access_table, new_cap * sizeof(ram_file_access_t));
   if (!new_table) {
      errno = ENOMEM;
      return -1;
   }
   memset(new_table + old_cap, 0, (new_cap - old_cap) * sizeof(ram_file_access_t));
   g_access_table = new_table;
   g_access_capacity = new_cap;
   return 0;
}

static ram_catalog_t *create_catalog_entry_generic(const char *path, size_t hash, int is_dir) {
   if (g_free_top == 0) {
      if (expand_files_capacity() != 0) {
         return NULL;
      }
   }
   if ((g_files_used + 1) * 2 > (g_table_mask + 1)) {
      if (resize_catalog_table() != 0) {
         return NULL;
      }
   }
   char *path_copy = strdup(path);
   if (!path_copy) {
      errno = ENOMEM;
      return NULL;
   }
   ram_file_t *file = g_files + g_free_stack[--g_free_top];
   file->first_chunk = NULL;
   file->chunks = NULL;
   file->chunks_count = 0;
   file->chunks_capacity = 0;
   file->size = 0;
   file->is_dir = is_dir;
   file->is_inlined = is_dir ? 0 : 1;
   size_t i = hash & g_table_mask;
   while (g_table[i].path != NULL) {
      i = (i + 1) & g_table_mask;
   }
   g_table[i].path = path_copy;
   g_table[i].file = file;
   g_table[i].hash = hash;
   catalog_depth_add(path);
   g_files_used++;
   if (g_files_used > g_peak_files) {
      g_peak_files = g_files_used;
   }
   return g_table + i;
}

ram_catalog_t *create_entry_in_catalog(const char *path, size_t hash) { return create_catalog_entry_generic(path, hash, 0); }

ram_catalog_t *create_dir_entry_in_catalog(const char *path, size_t hash) { return create_catalog_entry_generic(path, hash, 1); }

int is_dir_prefix(const char *clean_path) {
   if (!g_table || g_files_used == 0) {
      return 0;
   }
   unsigned slashes = count_slashes(clean_path);
   if (slashes >= g_max_depth) {
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
void remove_from_catalog(size_t i) {
   if (g_table[i].path != NULL) {
      catalog_depth_remove(g_table[i].path);
      free(g_table[i].path);
      g_table[i].path = NULL;
   }
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
ram_file_access_t *get_access(rfs_handle_t fd) {
   if ((g_files == NULL) || (g_access_table == NULL)) {
      errno = ENODEV;
      return NULL;
   }
   if (fd < 0 || !is_rfs_fd(fd)) {
      errno = EBADF;
      return NULL;
   }
   unsigned slot = fd_to_slot(fd);
   if (g_access_table[slot].file == NULL) {
      errno = EBADF;
      return NULL;
   }
   return g_access_table + slot;
}

/* =========================================================================
 * Normalisation et résolution des chemins
 * ========================================================================= */

static int handle_dotdot_component(char *dst, size_t *dst_idx, size_t root_idx, int is_absolute, size_t dst_size) {
   if (*dst_idx > root_idx) {
      size_t prev = *dst_idx - 1;
      while (prev > root_idx && dst[prev] != '/') {
         prev--;
      }
      *dst_idx = (dst[prev] == '/' && prev > root_idx) ? prev : root_idx;
   } else if (!is_absolute) {
      if (*dst_idx > root_idx) {
         if (*dst_idx + 1 >= dst_size) {
            errno = ENAMETOOLONG;
            return -1;
         }
         dst[(*dst_idx)++] = '/';
      }
      if (*dst_idx + 2 >= dst_size) {
         errno = ENAMETOOLONG;
         return -1;
      }
      dst[(*dst_idx)++] = '.';
      dst[(*dst_idx)++] = '.';
   }
   return 0;
}

static int append_path_component(char *dst, size_t *dst_idx, size_t root_idx, const char *comp_start, size_t comp_len,
                                 size_t dst_size) {
   if (*dst_idx > root_idx && dst[*dst_idx - 1] != '/') {
      if (*dst_idx + 1 >= dst_size) {
         errno = ENAMETOOLONG;
         return -1;
      }
      dst[(*dst_idx)++] = '/';
   }
   if (*dst_idx + comp_len >= dst_size) {
      errno = ENAMETOOLONG;
      return -1;
   }
   memcpy(dst + *dst_idx, comp_start, comp_len);
   *dst_idx += comp_len;
   return 0;
}

static inline int is_path_separator(char c) { return c == '/' || c == '\\'; }

static int parse_root_prefix(const char *src, size_t *src_idx, char *dst, size_t *dst_idx) {
   /* Lettre de lecteur Windows éventuelle (ex: "C:" ou "c:\") */
   if (isalpha((unsigned char)src[0]) && src[1] == ':') {
      dst[(*dst_idx)++] = (char)toupper((unsigned char)src[0]);
      dst[(*dst_idx)++] = ':';
      *src_idx = 2;
   }

   int is_absolute = is_path_separator(src[*src_idx]);
   if (is_absolute) {
      dst[(*dst_idx)++] = '/';
      *src_idx = (size_t)(skip_slashes(src + *src_idx) - src);
   }
   return is_absolute;
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
   int is_absolute = parse_root_prefix(src, &src_idx, dst, &dst_idx);
   size_t root_idx = dst_idx;

   while (src[src_idx] != '\0') {
      src_idx = (size_t)(skip_slashes(src + src_idx) - src);
      if (src[src_idx] == '\0') {
         break;
      }
      const char *comp_start = src + src_idx;
      while (src[src_idx] != '\0' && !is_path_separator(src[src_idx])) {
         src_idx++;
      }
      size_t comp_len = (size_t)(src + src_idx - comp_start);

      /* Composant "." ignoré */
      if (comp_len == 1 && comp_start[0] == '.') {
         continue;
      }

      /* Composant ".." */
      if (comp_len == 2 && comp_start[0] == '.' && comp_start[1] == '.') {
         if (handle_dotdot_component(dst, &dst_idx, root_idx, is_absolute, dst_size) != 0) {
            return -1;
         }
         continue;
      }

      /* Composant normal */
      if (append_path_component(dst, &dst_idx, root_idx, comp_start, comp_len, dst_size) != 0) {
         return -1;
      }
   }

   if (dst_idx == 0) {
      dst[dst_idx++] = '.';
   }
   dst[dst_idx] = '\0';
   return 0;
}

int resolve_path(const char *src, char *dst, size_t dst_size) {
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

/* =========================================================================
 * Opérations de stockage paginé et inliné (4-Tier Chunked & SFO Storage)
 * ========================================================================= */

unsigned char *rfs_file_get_chunk(const ram_file_t *file, size_t idx) {
   if (!file) {
      return NULL;
   }
   if (file->is_inlined) {
      return (idx == 0) ? (unsigned char *)file->inline_data : NULL;
   }
   if (idx >= file->chunks_count) {
      return NULL;
   }
   if (idx == 0) {
      return file->first_chunk;
   }
   return file->chunks ? file->chunks[idx] : NULL;
}

void rfs_file_free_storage(ram_file_t *file) {
   if (!file) {
      return;
   }
   if (file->first_chunk) {
      release_chunk(file->first_chunk);
      file->first_chunk = NULL;
   }
   if (file->chunks) {
      for (size_t i = 1; i < file->chunks_count; i++) {
         release_chunk(file->chunks[i]);
      }
      free(file->chunks);
      file->chunks = NULL;
   }
   file->chunks_count = 0;
   file->chunks_capacity = 0;
   file->size = 0;
   file->is_inlined = 1;
}

static int rfs_file_ensure_capacity(ram_file_t *file, size_t needed_size) {
   if (needed_size == 0) {
      return 0;
   }
   size_t needed_chunks = (needed_size + RAMFS_CHUNK_SIZE - 1) >> RAMFS_CHUNK_SHIFT;
   if (needed_chunks <= file->chunks_count) {
      return 0;
   }
   if (file->chunks_count == 0) {
      unsigned char *c = allocate_chunk();
      if (!c) {
         errno = ENOMEM;
         return -1;
      }
      file->first_chunk = c;
      file->chunks_count = 1;
   }
   if (needed_chunks <= 1) {
      return 0;
   }
   if (needed_chunks > file->chunks_capacity) {
      size_t new_cap = file->chunks_capacity ? file->chunks_capacity * 2 : 4;
      while (new_cap < needed_chunks) {
         new_cap *= 2;
      }
      unsigned char **new_chunks = realloc(file->chunks, new_cap * sizeof(unsigned char *));
      if (!new_chunks) {
         errno = ENOMEM;
         return -1;
      }
      if (file->chunks == NULL) {
         new_chunks[0] = file->first_chunk;
      }
      file->chunks = new_chunks;
      file->chunks_capacity = new_cap;
   }
   while (file->chunks_count < needed_chunks) {
      unsigned char *c = allocate_chunk();
      if (!c) {
         errno = ENOMEM;
         return -1;
      }
      file->chunks[file->chunks_count++] = c;
   }
   return 0;
}

static void rfs_file_zero_range(ram_file_t *file, size_t start, size_t end) {
   size_t cur = start;
   while (cur < end) {
      size_t chunk_idx = cur >> RAMFS_CHUNK_SHIFT;
      size_t chunk_off = cur & RAMFS_CHUNK_MASK;
      size_t chunk_avail = RAMFS_CHUNK_SIZE - chunk_off;
      size_t to_zero = (end - cur) < chunk_avail ? (end - cur) : chunk_avail;
      unsigned char *chunk = rfs_file_get_chunk(file, chunk_idx);
      if (chunk) {
         memset(chunk + chunk_off, 0, to_zero);
      }
      cur += to_zero;
   }
}

size_t rfs_file_read_bytes(const ram_file_t *file, size_t offset, void *buf, size_t count) {
   if (!file || !buf || count == 0 || offset >= file->size) {
      return 0;
   }
   size_t available = file->size - offset;
   size_t to_read = count < available ? count : available;
   if (file->is_inlined) {
      memcpy(buf, file->inline_data + offset, to_read);
      return to_read;
   }
   size_t read_bytes = 0;
   char *dst = (char *)buf;
   while (read_bytes < to_read) {
      size_t cur = offset + read_bytes;
      size_t chunk_idx = cur >> RAMFS_CHUNK_SHIFT;
      size_t chunk_off = cur & RAMFS_CHUNK_MASK;
      size_t chunk_avail = RAMFS_CHUNK_SIZE - chunk_off;
      size_t n = (to_read - read_bytes) < chunk_avail ? (to_read - read_bytes) : chunk_avail;
      const unsigned char *chunk = rfs_file_get_chunk(file, chunk_idx);
      if (chunk) {
         memcpy(dst + read_bytes, chunk + chunk_off, n);
      }
      read_bytes += n;
   }
   return read_bytes;
}

int rfs_file_write_bytes(ram_file_t *file, size_t offset, const void *buf, size_t count) {
   if (!file || (!buf && count > 0)) {
      errno = EINVAL;
      return -1;
   }
   if (count == 0) {
      return 0;
   }
   size_t target_end = offset + count;
   if (file->is_inlined && target_end <= RAMFS_INLINE_CAPACITY) {
      if (offset > file->size) {
         memset(file->inline_data + file->size, 0, offset - file->size);
      }
      memcpy(file->inline_data + offset, buf, count);
      if (target_end > file->size) {
         file->size = target_end;
      }
      return 0;
   }
   if (file->is_inlined) {
      if (rfs_file_ensure_capacity(file, target_end) != 0) {
         return -1;
      }
      file->is_inlined = 0;
      if (file->size > 0) {
         memcpy(file->first_chunk, file->inline_data, file->size);
      }
      if (offset > file->size) {
         rfs_file_zero_range(file, file->size, offset);
      }
   } else if (target_end > file->size) {
      if (rfs_file_ensure_capacity(file, target_end) != 0) {
         return -1;
      }
      if (offset > file->size) {
         rfs_file_zero_range(file, file->size, offset);
      }
   }
   size_t written = 0;
   const char *src = (const char *)buf;
   while (written < count) {
      size_t cur = offset + written;
      size_t chunk_idx = cur >> RAMFS_CHUNK_SHIFT;
      size_t chunk_off = cur & RAMFS_CHUNK_MASK;
      size_t chunk_avail = RAMFS_CHUNK_SIZE - chunk_off;
      size_t n = (count - written) < chunk_avail ? (count - written) : chunk_avail;
      unsigned char *chunk = rfs_file_get_chunk(file, chunk_idx);
      if (chunk) {
         memcpy(chunk + chunk_off, src + written, n);
      }
      written += n;
   }
   if (target_end > file->size) {
      file->size = target_end;
   }
   return 0;
}

int rfs_file_truncate(ram_file_t *file, size_t new_len) {
   if (!file) {
      errno = EINVAL;
      return -1;
   }
   if (new_len == file->size) {
      return 0;
   }
   if (file->is_inlined) {
      if (new_len <= RAMFS_INLINE_CAPACITY) {
         if (new_len > file->size) {
            memset(file->inline_data + file->size, 0, new_len - file->size);
         }
         file->size = new_len;
         return 0;
      }
      if (rfs_file_ensure_capacity(file, new_len) != 0) {
         return -1;
      }
      file->is_inlined = 0;
      if (file->size > 0) {
         memcpy(file->first_chunk, file->inline_data, file->size);
      }
      rfs_file_zero_range(file, file->size, new_len);
      file->size = new_len;
      return 0;
   }
   if (new_len > file->size) {
      if (rfs_file_ensure_capacity(file, new_len) != 0) {
         return -1;
      }
      rfs_file_zero_range(file, file->size, new_len);
      file->size = new_len;
      return 0;
   }
   size_t needed_chunks = (new_len + RAMFS_CHUNK_SIZE - 1) >> RAMFS_CHUNK_SHIFT;
   while (file->chunks_count > needed_chunks) {
      size_t idx = --file->chunks_count;
      if (idx == 0) {
         release_chunk(file->first_chunk);
         file->first_chunk = NULL;
      } else if (file->chunks) {
         release_chunk(file->chunks[idx]);
         file->chunks[idx] = NULL;
      }
   }
   if (needed_chunks <= 1 && file->chunks != NULL) {
      free(file->chunks);
      file->chunks = NULL;
      file->chunks_capacity = 0;
   }
   file->size = new_len;
   return 0;
}

int rfs_file_get_byte(const ram_file_t *file, size_t offset) {
   if (!file || offset >= file->size) {
      return -1;
   }
   if (file->is_inlined) {
      return (int)file->inline_data[offset];
   }
   size_t chunk_idx = offset >> RAMFS_CHUNK_SHIFT;
   size_t chunk_off = offset & RAMFS_CHUNK_MASK;
   const unsigned char *chunk = rfs_file_get_chunk(file, chunk_idx);
   return chunk ? (int)chunk[chunk_off] : -1;
}

int rfs_file_put_byte(ram_file_t *file, size_t offset, unsigned char c) {
   if (!file || offset >= file->size) {
      return -1;
   }
   if (file->is_inlined) {
      file->inline_data[offset] = c;
      return 0;
   }
   size_t chunk_idx = offset >> RAMFS_CHUNK_SHIFT;
   size_t chunk_off = offset & RAMFS_CHUNK_MASK;
   unsigned char *chunk = rfs_file_get_chunk(file, chunk_idx);
   if (!chunk) {
      return -1;
   }
   chunk[chunk_off] = c;
   return 0;
}

void fill_stat_buf(const ram_file_t *file, struct stat *buf) {
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

/* =========================================================================
 * Initialisation, destruction et statistiques
 * ========================================================================= */

int rfs_init(unsigned files_count, unsigned access_count) {
   rfs_destroy();
   if (files_count == 0) {
      files_count = 32;
   }
   if (access_count == 0) {
      access_count = 16;
   }
   size_t table_size = MIN_TABLE_SIZE;
   while (table_size < (size_t)files_count * 2) {
      table_size *= 2; /* facteur de charge <= 0,5 */
   }
   g_files = calloc(files_count, sizeof(ram_file_t));
   g_free_stack = malloc(files_count * sizeof(unsigned));
   g_table = calloc(table_size, sizeof(ram_catalog_t));
   g_access_table = calloc(access_count, sizeof(ram_file_access_t));
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
   g_max_depth = 0;
   memset(g_depth_counts, 0, sizeof(g_depth_counts));
   snprintf(g_cwd, sizeof(g_cwd), "/");
   const char *env_mount = getenv("RAMFS_MOUNT_PREFIX");
   if (env_mount && env_mount[0] != '\0') {
      rfs_mount(env_mount);
   } else {
      rfs_unmount();
   }
   /* indices poussés en ordre décroissant : le 0 sort en premier */
   for (unsigned i = 0; i < files_count; i++) {
      g_free_stack[i] = files_count - 1 - i;
   }
   g_free_top = files_count;
   /* Préchauffage du pool (Axe 4) : 1 slab de 16 blocs de 64 Ko (1 Mo) */
   allocate_slab();
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
         rfs_file_free_storage(g_files + i);
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
   if (g_slab_list) {
      for (size_t i = 0; i < g_slab_count; i++) {
         free(g_slab_list[i]);
      }
      free(g_slab_list);
      g_slab_list = NULL;
      g_slab_count = 0;
      g_slab_capacity = 0;
   }
   if (g_chunk_pool) {
      free(g_chunk_pool);
      g_chunk_pool = NULL;
      g_chunk_pool_top = 0;
      g_chunk_pool_capacity = 0;
   }
   snprintf(g_cwd, sizeof(g_cwd), "/");
   rfs_unmount();
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
   g_max_depth = 0;
   memset(g_depth_counts, 0, sizeof(g_depth_counts));
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
