#include "ramfile.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC target("sse4.2")
#endif
#include <smmintrin.h>
#endif

#ifdef _WIN32
#include <direct.h>
#define mkdir_portable(path) _mkdir(path)
typedef intptr_t ssize_t;
#else
#define mkdir_portable(path) mkdir(path, 0755)
#endif

#define RAMFILE_INITIAL_CAPACITY 1024
#define MIN_TABLE_SIZE 8

typedef struct {
   unsigned char *data;
   size_t size;
   size_t capacity;
} ram_file_t;

/* Une entrée de la table de hachage. path == NULL signifie "case vide". */
typedef struct {
   char *path;
   ram_file_t *file;
   size_t hash;
} ram_catalog_t;

typedef struct {
   ram_file_t *file;
   off_t offset;
   int flags;
} ram_file_access_t;

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

void rf_init(unsigned files_count, unsigned access_count) {
   rf_destroy();
   size_t table_size = MIN_TABLE_SIZE;
   while (table_size < (size_t)files_count * 2) {
      table_size *= 2; /* facteur de charge <= 0,5 */
   }
   g_files = calloc(files_count ? files_count : 1, sizeof(ram_file_t));
   g_free_stack = malloc((files_count ? files_count : 1) * sizeof(unsigned));
   g_table = calloc(table_size, sizeof(ram_catalog_t));
   g_access_table = calloc(access_count ? access_count : 1, sizeof(ram_file_access_t));
   if (!g_files || !g_free_stack || !g_table || !g_access_table) {
      rf_destroy();
      return;
   }
   g_files_capacity = files_count;
   g_table_mask = table_size - 1;
   g_access_capacity = access_count;
   /* indices poussés en ordre décroissant : le 0 sort en premier */
   for (unsigned i = 0; i < files_count; i++) {
      g_free_stack[i] = files_count - 1 - i;
   }
   g_free_top = files_count;
}

void rf_destroy(void) {
   if (g_table) {
      for (size_t i = 0; i <= g_table_mask; i++) {
         free(g_table[i].path);
      }
   }
   if (g_files) {
      for (unsigned i = 0; i < g_files_capacity; i++) {
         free(g_files[i].data);
      }
   }
   free(g_table);
   free(g_files);
   free(g_free_stack);
   free(g_access_table);
   g_table = NULL;
   g_table_mask = 0;
   g_files = NULL;
   g_free_stack = NULL;
   g_free_top = 0;
   g_files_capacity = 0;
   g_access_table = NULL;
   g_access_capacity = 0;
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
   unsigned char *data = calloc(1, RAMFILE_INITIAL_CAPACITY);
   if (!path_copy || !data) {
      free(path_copy);
      free(data);
      errno = ENOMEM;
      return NULL;
   }
   ram_file_t *file = g_files + g_free_stack[--g_free_top];
   file->data = data;
   file->size = 0;
   file->capacity = RAMFILE_INITIAL_CAPACITY;
   size_t i = hash & g_table_mask;
   while (g_table[i].path != NULL) {
      i = (i + 1) & g_table_mask;
   }
   g_table[i].path = path_copy;
   g_table[i].file = file;
   g_table[i].hash = hash;
   return g_table + i;
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
static ram_file_access_t *get_access(rf_handle_t fd) {
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

rf_handle_t rf_open(const char *pathname, int flags, ...) {
   if ((g_files == NULL) || (g_access_table == NULL)) {
      errno = ENODEV;
      return RF_INVALID_HANDLE;
   }
   if (!pathname) {
      errno = EINVAL;
      return RF_INVALID_HANDLE;
   }
   int access_mode = flags & O_ACCMODE;
   size_t hash = hash_path(pathname);
   ram_catalog_t *entry = search_in_catalog(pathname, hash);
   ram_file_t *file = NULL;
   if (entry) {
      file = entry->file;
      if ((flags & O_CREAT) && (flags & O_EXCL)) {
         errno = EEXIST;
         return RF_INVALID_HANDLE;
      }
   } else {
      if (flags & O_CREAT) {
         entry = create_entry_in_catalog(pathname, hash);
         if (entry == NULL) {
            return RF_INVALID_HANDLE;
         }
         file = entry->file;
      } else {
         errno = ENOENT;
         return RF_INVALID_HANDLE;
      }
   }
   rf_handle_t free_slot = RF_INVALID_HANDLE;
   for (unsigned i = 0; i < g_access_capacity; i++) {
      const ram_file_access_t *access = g_access_table + i;
      if (access->file == file) {
         int active_mode = access->flags & O_ACCMODE;
         if (active_mode != O_RDONLY || access_mode != O_RDONLY) {
            errno = EBUSY;
            return RF_INVALID_HANDLE;
         }
      } else if ((free_slot == RF_INVALID_HANDLE) && (access->file == NULL)) {
         free_slot = (rf_handle_t)i;
      }
   }
   if (free_slot == RF_INVALID_HANDLE) {
      fprintf(stderr, "Capacité d'accès atteinte !\n");
      errno = ENOMEM;
      return RF_INVALID_HANDLE;
   }
   if (flags & O_TRUNC) {
      file->size = 0;
   }
   ram_file_access_t *access = g_access_table + free_slot;
   access->file = file;
   access->flags = flags;
   access->offset = (flags & O_APPEND) ? (off_t)file->size : 0;
   return free_slot;
}

ssize_t rf_read(rf_handle_t fd, void *buf, size_t count) {
   ram_file_access_t *acc = get_access(fd);
   if (!acc || (acc->flags & O_ACCMODE) == O_WRONLY) {
      if (acc) {
         errno = EBADF;
      }
      return -1;
   }
   ram_file_t *file = acc->file;
   if (acc->offset >= (off_t)file->size) {
      return 0;
   }
   size_t available = file->size - (size_t)acc->offset;
   size_t to_read = count > available ? available : count;
   memcpy(buf, file->data + acc->offset, to_read);
   acc->offset += (off_t)to_read;
   return (ssize_t)to_read;
}

ssize_t rf_write(rf_handle_t fd, const void *buf, size_t count) {
   ram_file_access_t *acc = get_access(fd);
   if (!acc || (acc->flags & O_ACCMODE) == O_RDONLY) {
      if (acc) {
         errno = EBADF;
      }
      return -1;
   }
   ram_file_t *file = acc->file;
   if (acc->flags & O_APPEND) {
      acc->offset = (off_t)file->size;
   }
   size_t required_size = (size_t)acc->offset + count;
   if (required_size > file->capacity) {
      size_t new_capacity = file->capacity == 0 ? RAMFILE_INITIAL_CAPACITY : file->capacity * 2;
      while (new_capacity < required_size) {
         new_capacity *= 2;
      }
      unsigned char *new_data = realloc(file->data, new_capacity);
      if (!new_data) {
         errno = ENOMEM;
         return -1;
      }
      file->data = new_data;
      file->capacity = new_capacity;
   }
   if ((size_t)acc->offset > file->size) {
      /* écriture après un lseek au-delà de la fin : le trou est rempli de zéros */
      memset(file->data + file->size, 0, (size_t)acc->offset - file->size);
   }
   memcpy(file->data + acc->offset, buf, count);
   acc->offset += (off_t)count;
   if ((size_t)acc->offset > file->size) {
      file->size = (size_t)acc->offset;
   }
   return (ssize_t)count;
}

off_t rf_lseek(rf_handle_t fd, off_t offset, int whence) {
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      return -1;
   }
   off_t new_offset = acc->offset;
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
   acc->offset = new_offset;
   return acc->offset;
}

int rf_close(rf_handle_t fd) {
   ram_file_access_t *acc = get_access(fd);
   if (!acc) {
      return -1;
   }
   memset(acc, 0, sizeof(ram_file_access_t));
   return 0;
}

int rf_unlink(const char *pathname) {
   if (!pathname) {
      errno = EINVAL;
      return -1;
   }
   ram_catalog_t *entry = search_in_catalog(pathname, hash_path(pathname));
   if (entry == NULL) {
      errno = ENOENT;
      return -1;
   }
   ram_file_t *file = entry->file;
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
   return 0;
}

#define STDIO_HANDLE_BIAS ((uintptr_t)0xBA5E0000)

static FILE *handle_to_file_ptr(rf_handle_t handle) {
   if (handle == RF_INVALID_HANDLE) {
      return NULL;
   }
   return (FILE *)(STDIO_HANDLE_BIAS + (uintptr_t)handle);
}

static rf_handle_t file_ptr_to_handle(FILE *stream) {
   if (!stream) {
      return RF_INVALID_HANDLE;
   }
   uintptr_t addr = (uintptr_t)stream;
   /* Seule la plage [BIAS, BIAS + access_count) désigne un faux FILE* de ramfile.
      Tout le reste (stdin, stdout, stderr, vrai FILE*) est rejeté. */
   if (addr < STDIO_HANDLE_BIAS || addr - STDIO_HANDLE_BIAS >= g_access_capacity) {
      return RF_INVALID_HANDLE;
   }
   return (rf_handle_t)(addr - STDIO_HANDLE_BIAS);
}

FILE *rf_fopen(const char *pathname, const char *mode) {
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
   rf_handle_t handle = rf_open(pathname, flags, 0644);
   return handle_to_file_ptr(handle);
}

size_t rf_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
   rf_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RF_INVALID_HANDLE || size == 0 || nmemb == 0) {
      return 0;
   }
   ssize_t bytes_read = rf_read(handle, ptr, size * nmemb);
   if (bytes_read <= 0) {
      return 0;
   }
   return (size_t)bytes_read / size;
}

size_t rf_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
   rf_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RF_INVALID_HANDLE || size == 0 || nmemb == 0) {
      return 0;
   }
   ssize_t bytes_written = rf_write(handle, ptr, size * nmemb);
   if (bytes_written <= 0) {
      return 0;
   }
   return (size_t)bytes_written / size;
}

int rf_fseek(FILE *stream, long offset, int whence) {
   rf_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RF_INVALID_HANDLE) {
      errno = EBADF;
      return -1;
   }
   off_t res = rf_lseek(handle, (off_t)offset, whence);
   return (res == (off_t)-1) ? -1 : 0;
}

long rf_ftell(FILE *stream) {
   rf_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RF_INVALID_HANDLE) {
      errno = EBADF;
      return -1;
   }
   off_t current_offset = rf_lseek(handle, 0, SEEK_CUR);
   return (long)current_offset;
}

int rf_fprintf(FILE *stream, const char *format, ...) {
   rf_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RF_INVALID_HANDLE || !format) {
      errno = EBADF;
      return -1;
   }
   va_list args;
   va_start(args, format);
   int len = vsnprintf(NULL, 0, format, args);
   va_end(args);
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
   va_start(args, format);
   vsnprintf(buffer, (size_t)len + 1, format, args);
   va_end(args);
   ssize_t written = rf_write(handle, buffer, (size_t)len);
   free(buffer);
   return (written < 0) ? -1 : (int)written;
}

int rf_fscanf(FILE *stream, const char *format, ...) {
   rf_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RF_INVALID_HANDLE || !format) {
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
   if (acc->offset >= (off_t)file->size) {
      return EOF;
   }
   /* vsscanf exige une chaîne terminée par '\0' : on travaille sur une copie. */
   size_t remaining = file->size - (size_t)acc->offset;
   char *text = malloc(remaining + 1);
   if (!text) {
      errno = ENOMEM;
      return -1;
   }
   memcpy(text, file->data + acc->offset, remaining);
   text[remaining] = '\0';
   size_t fmt_len = strlen(format);
   va_list args;
   va_start(args, format);
   int fields_read = vsscanf(text, format, args);
   va_end(args);
   if (fields_read == EOF) {
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
      sscanf(text, suppress_format, &bytes_consumed);
      if (bytes_consumed > 0) {
         acc->offset += bytes_consumed;
      }
      free(suppress_format);
   }
   free(text);
   return fields_read;
}

int rf_fclose(FILE *stream) {
   rf_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RF_INVALID_HANDLE) {
      errno = EBADF;
      return -1;
   }
   return rf_close(handle);
}

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

int rf_flush_to_disk(const char *dest_dir) {
   if (!dest_dir) {
      errno = EINVAL;
      return -1;
   }
   mkdir_portable(dest_dir);
   if (!g_table) {
      return 0;
   }
   for (size_t i = 0; i <= g_table_mask; i++) {
      const ram_catalog_t *entry = g_table + i;
      if (entry->path == NULL) {
         continue;
      }
      const ram_file_t *file = entry->file;
      if (file->size == 0 || file->data == NULL) {
         continue;
      }
      char real_filepath[512];
      snprintf(real_filepath, sizeof(real_filepath), "%s/%s", dest_dir, entry->path);
      char parent_dir[512] = {0};
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
      if (f) {
         fwrite(file->data, 1, file->size, f);
         fclose(f);
      }
   }
   return 0;
}
