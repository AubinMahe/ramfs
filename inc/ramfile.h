#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef BUILDING_RAMFILE_DLL
#define RAMFILE_API __declspec(dllexport)
#else
#define RAMFILE_API __declspec(dllimport)
#endif
#else
#define RAMFILE_API __attribute__((visibility("default")))
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

typedef int rf_handle_t;

#define RF_INVALID_HANDLE (-1)

RAMFILE_API void rf_init(unsigned file_count, unsigned access_count);
RAMFILE_API void rf_destroy(void);

RAMFILE_API rf_handle_t rf_open(const char *pathname, int flags, ...);
RAMFILE_API ssize_t rf_read(rf_handle_t fd, void *buf, size_t count);
RAMFILE_API ssize_t rf_write(rf_handle_t fd, const void *buf, size_t count);
RAMFILE_API off_t rf_lseek(rf_handle_t fd, off_t offset, int whence);
RAMFILE_API int rf_close(rf_handle_t fd);
RAMFILE_API int rf_unlink(const char *pathname);

RAMFILE_API FILE *rf_fopen(const char *pathname, const char *mode);
RAMFILE_API size_t rf_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
RAMFILE_API size_t rf_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
RAMFILE_API int rf_fseek(FILE *stream, long offset, int whence);
RAMFILE_API long rf_ftell(FILE *stream);
RAMFILE_API int rf_fprintf(FILE *stream, const char *format, ...);
RAMFILE_API int rf_fscanf(FILE *stream, const char *format, ...);
RAMFILE_API int rf_fclose(FILE *stream);
/*
 * Fonctions non redirigées : fgets, fputs, fgetc, fputc, feof, ferror, fflush, rewind, remove, rename, stat, access, etc.
 */

RAMFILE_API int rf_flush_to_disk(const char *dest_dir);

#ifdef USE_RAMFILE

#define open(...) rf_open(__VA_ARGS__)
#define read(fd, buf, count) rf_read(fd, buf, count)
#define write(fd, buf, count) rf_write(fd, buf, count)
#define lseek(fd, offset, wh) rf_lseek(fd, offset, wh)
#define close(fd) rf_close(fd)
#define unlink(path) rf_unlink(path)

#define fopen(path, mode) rf_fopen(path, mode)
#define fread(ptr, sz, n, st) rf_fread(ptr, sz, n, st)
#define fwrite(ptr, sz, n, st) rf_fwrite(ptr, sz, n, st)
#define fseek(st, off, wh) rf_fseek(st, off, wh)
#define ftell(st) rf_ftell(st)
#define fprintf(...) rf_fprintf(__VA_ARGS__)
#define fscanf(...) rf_fscanf(__VA_ARGS__)
#define fclose(st) rf_fclose(st)

#endif
