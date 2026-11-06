#include "ramfs_core.h"
#include "ramfs_vfs.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STDIO_HANDLE_BIAS ((uintptr_t)0xBA5E0000)

/* =========================================================================
 * Variables et fonctions de conversion - privées au module stdio
 * ========================================================================= */

static unsigned g_tmpfile_counter = 0;

static inline FILE *handle_to_file_ptr(rfs_handle_t handle) {
   if (handle == RFS_INVALID_HANDLE) {
      return NULL;
   }
   return (FILE *)(STDIO_HANDLE_BIAS + (uintptr_t)handle);
}

static inline rfs_handle_t file_ptr_to_handle(FILE *stream) {
   if (!stream) {
      return RFS_INVALID_HANDLE;
   }
   uintptr_t addr = (uintptr_t)stream;
   if (addr < STDIO_HANDLE_BIAS || addr - STDIO_HANDLE_BIAS >= g_access_capacity) {
      return RFS_INVALID_HANDLE;
   }
   return slot_to_fd((unsigned)(addr - STDIO_HANDLE_BIAS));
}

/* =========================================================================
 * Fonctions de cycle de vie et flux (FILE*)
 * ========================================================================= */

FILE *rfs_fopen(const char *pathname, const char *mode) {
   if (!pathname || !mode) {
      errno = EINVAL;
      return NULL;
   }
   char clean_path[PATH_MAX];
   if (resolve_path(pathname, clean_path, sizeof(clean_path)) != 0) {
      return NULL;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(clean_path, ram_rel, sizeof(ram_rel))) {
#undef fopen
      return fopen(pathname, mode);
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
   rfs_handle_t handle = rfs_open(clean_path, flags, 0644);
   if (handle == RFS_INVALID_HANDLE) {
      return NULL;
   }
   return handle_to_file_ptr(fd_to_slot(handle));
}

size_t rfs_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
   if (!stream || size == 0 || nmemb == 0) {
      if (!stream) {
         errno = EBADF;
      }
      return 0;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef fread
         return fread(ptr, size, nmemb, stream);
      }
      errno = EBADF;
      return 0;
   }
   ssize_t bytes_read = rfs_read(handle, ptr, size * nmemb);
   if (bytes_read <= 0) {
      return 0;
   }
   return (size_t)bytes_read / size;
}

size_t rfs_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
   if (!stream || size == 0 || nmemb == 0) {
      if (!stream) {
         errno = EBADF;
      }
      return 0;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef fwrite
         return fwrite(ptr, size, nmemb, stream);
      }
      errno = EBADF;
      return 0;
   }
   ssize_t bytes_written = rfs_write(handle, ptr, size * nmemb);
   if (bytes_written <= 0) {
      return 0;
   }
   return (size_t)bytes_written / size;
}

int rfs_fseek(FILE *stream, long offset, int whence) {
   if (!stream) {
      errno = EBADF;
      return -1;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef fseek
         return fseek(stream, offset, whence);
      }
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
   if (!stream) {
      errno = EBADF;
      return -1;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef ftell
         return ftell(stream);
      }
      errno = EBADF;
      return -1;
   }
   off_t current_offset = rfs_lseek(handle, 0, SEEK_CUR);
   return (long)current_offset;
}

void rfs_rewind(FILE *stream) {
   if (!stream) {
      return;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef rewind
         rewind(stream);
         return;
      }
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
      for (unsigned i = 0; i < g_access_capacity; i++) {
         if (g_access_table[i].file != NULL) {
            g_access_table[i].error = 0;
         }
      }
      if (g_mount_len > 0) {
#undef fflush
         fflush(NULL);
      }
      return 0;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (stream == stdout || stream == stderr || stream == stdin || g_mount_len > 0) {
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
   if (!stream) {
      return 0;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef feof
         return feof(stream);
      }
      return 0;
   }
   ram_file_access_t *acc = get_access(handle);
   return acc ? acc->eof : 0;
}

int rfs_ferror(FILE *stream) {
   if (!stream) {
      return 0;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef ferror
         return ferror(stream);
      }
      return 0;
   }
   ram_file_access_t *acc = get_access(handle);
   return acc ? acc->error : 0;
}

void rfs_clearerr(FILE *stream) {
   if (!stream) {
      return;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef clearerr
         clearerr(stream);
         return;
      }
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
      if (stream == stdin || stream == stdout || stream == stderr || g_mount_len > 0) {
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
   if (c == EOF || !stream) {
      if (!stream) {
         errno = EBADF;
      }
      return EOF;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef ungetc
         return ungetc(c, stream);
      }
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
   if (!stream) {
      errno = EBADF;
      return EOF;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef fgetc
         return fgetc(stream);
      }
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
   if (!s || size <= 0 || !stream) {
      errno = !stream ? EBADF : EINVAL;
      return NULL;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef fgets
         return fgets(s, size, stream);
      }
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
   if (!stream) {
      errno = EBADF;
      return EOF;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef fputc
         return fputc(c, stream);
      }
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
   if (!s || !stream) {
      errno = !stream ? EBADF : EINVAL;
      return EOF;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef fputs
         return fputs(s, stream);
      }
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
      if (g_mount_len > 0) {
#undef vfprintf
         return vfprintf(stream, format, ap);
      }
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

int rfs_vfscanf(FILE *stream, const char *format, va_list ap) {
   if (!stream || !format) {
      errno = EINVAL;
      return -1;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (stream == stdin || g_mount_len > 0) {
#undef vfscanf
         return vfscanf(stream, format, ap);
      }
      errno = EBADF;
      return EOF;
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
   va_copy(args, ap);
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
               suppress_format[j++] = format[++i];
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

int rfs_fscanf(FILE *stream, const char *format, ...) {
   va_list args;
   va_start(args, format);
   int ret = rfs_vfscanf(stream, format, args);
   va_end(args);
   return ret;
}

int rfs_fclose(FILE *stream) {
   if (!stream) {
      errno = EBADF;
      return -1;
   }
   rfs_handle_t handle = file_ptr_to_handle(stream);
   if (handle == RFS_INVALID_HANDLE) {
      if (g_mount_len > 0) {
#undef fclose
         return fclose(stream);
      }
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
      if (g_mount_len > 0) {
#undef getdelim
#if !defined(_WIN32)
         return getdelim(lineptr, n, delimiter, stream);
#else
         if (*lineptr == NULL || *n == 0) {
            *n = 128;
            *lineptr = malloc(*n);
            if (!*lineptr) {
               errno = ENOMEM;
               return -1;
            }
         }
         size_t r = 0;
         int ch;
         while ((ch = fgetc(stream)) != EOF) {
            if (r + 2 > *n) {
               size_t new_n = *n * 2;
               char *new_ptr = realloc(*lineptr, new_n);
               if (!new_ptr) {
                  errno = ENOMEM;
                  return -1;
               }
               *lineptr = new_ptr;
               *n = new_n;
            }
            (*lineptr)[r++] = (char)ch;
            if (ch == delimiter) {
               break;
            }
         }
         if (r == 0 && (ch == EOF || feof(stream) || ferror(stream))) {
            return -1;
         }
         (*lineptr)[r] = '\0';
         return (ssize_t)r;
#endif
      }
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
