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
#include <sys/stat.h>
#include <sys/types.h>

typedef int rf_handle_t;

#define RF_INVALID_HANDLE (-1)

typedef struct {
   unsigned files_used;      /* Nombre actuel de fichiers en mémoire */
   unsigned peak_files;      /* Pic maximal de fichiers simultanés */
   unsigned files_capacity;  /* Capacité configurée lors de rf_init */
   unsigned access_used;     /* Nombre actuel de descripteurs ouverts */
   unsigned peak_access;     /* Pic maximal de descripteurs simultanés */
   unsigned access_capacity; /* Capacité configurée lors de rf_init */
} rf_stats_t;

/**
 * Initialise les structures de données de ramfile à partir des limites fournies.
 *
 * @param file_count nombre maximal de fichiers
 * @param access_count nombre maximal de descripteur de fichier (open, fopen)
 * @return 0 en cas de succès, -1 en cas d'erreur (errno positionné).
 */
RAMFILE_API int rf_init(unsigned file_count, unsigned access_count);

/**
 * Libère les ressources, réinitialise les descripteurs statiques.
 */
RAMFILE_API void rf_destroy(void);

/**
 * Récupère les métriques d'utilisation et de crête de la bibliothèque.
 *
 * @param stats pointeur vers la structure recevant les métriques
 * @return 0 en cas de succès, -1 si non initialisé (errno = ENODEV) ou stats NULL (errno = EINVAL).
 */
RAMFILE_API int rf_get_stats(rf_stats_t *stats);

/**
 * Affiche un rapport synthétique des jauges et pics d'utilisation.
 *
 * @param stream flux cible (si NULL, affiche sur stderr).
 */
RAMFILE_API void rf_dump_stats(FILE *stream);

RAMFILE_API rf_handle_t rf_open(const char *pathname, int flags, ...);
RAMFILE_API ssize_t rf_read(rf_handle_t fd, void *buf, size_t count);
RAMFILE_API ssize_t rf_write(rf_handle_t fd, const void *buf, size_t count);
RAMFILE_API off_t rf_lseek(rf_handle_t fd, off_t offset, int whence);
RAMFILE_API int rf_close(rf_handle_t fd);
RAMFILE_API int rf_unlink(const char *pathname);
RAMFILE_API int rf_remove(const char *pathname);
RAMFILE_API int rf_stat(const char *pathname, struct stat *buf);
RAMFILE_API int rf_fstat(rf_handle_t fd, struct stat *buf);
RAMFILE_API int rf_rename(const char *oldpath, const char *newpath);
RAMFILE_API int rf_access(const char *pathname, int mode);
RAMFILE_API int rf_isatty(rf_handle_t fd);
RAMFILE_API int rf_normalize_path(const char *src, char *dst, size_t dst_size);

RAMFILE_API FILE *rf_fopen(const char *pathname, const char *mode);
RAMFILE_API size_t rf_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
RAMFILE_API size_t rf_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
RAMFILE_API int rf_fseek(FILE *stream, long offset, int whence);
RAMFILE_API long rf_ftell(FILE *stream);
RAMFILE_API void rf_rewind(FILE *stream);
RAMFILE_API int rf_fflush(FILE *stream);
RAMFILE_API int rf_feof(FILE *stream);
RAMFILE_API int rf_ferror(FILE *stream);
RAMFILE_API void rf_clearerr(FILE *stream);
RAMFILE_API int rf_fileno(FILE *stream);
RAMFILE_API int rf_fgetc(FILE *stream);
RAMFILE_API int rf_ungetc(int c, FILE *stream);
RAMFILE_API char *rf_fgets(char *s, int size, FILE *stream);
RAMFILE_API int rf_fputc(int c, FILE *stream);
RAMFILE_API int rf_fputs(const char *s, FILE *stream);
RAMFILE_API int rf_fprintf(FILE *stream, const char *format, ...);
RAMFILE_API int rf_fscanf(FILE *stream, const char *format, ...);
RAMFILE_API int rf_fclose(FILE *stream);

/**
 * Sauvegarde une sélection de fichiers en mémoire vers une archive compressée (.tar.gz) ou vers le disque.
 *
 * @param paths tableau de chemins à sauvegarder (si NULL, tous les fichiers en mémoire sont sauvegardés).
 * @param path_count nombre d'éléments dans paths (si 0 et paths != NULL, le tableau est considéré terminé par NULL).
 * @param archive_path si non NULL, crée une archive compressée .tar.gz contenant les fichiers sélectionnés.
 * @param dest_dir si archive_path == NULL, répertoire racine de destination sur disque (si NULL, relatif au PWD).
 * @return 0 en cas de succès, -1 en cas d'erreur (errno positionné).
 */
RAMFILE_API int rf_save(const char *const *paths, size_t path_count, const char *archive_path, const char *dest_dir);

/**
 * Réciproque de rf_save : charge et peuple la mémoire de ramfile depuis une archive (.tar.gz) ou le disque.
 *
 * @param archive_path si non NULL, chemin de l'archive compressée .tar.gz à charger en mémoire.
 * @param src_dir si archive_path == NULL, répertoire racine source sur disque à charger (si NULL, relatif au PWD).
 * @param prefix préfixe optionnel à ajouter aux chemins extraits en mémoire (NULL pour conserver l'arborescence telle quelle).
 * @return le nombre de fichiers chargés en mémoire en cas de succès (>= 0), ou -1 en cas d'erreur (errno positionné).
 */
RAMFILE_API int rf_load(const char *archive_path, const char *src_dir, const char *prefix);

#ifdef USE_RAMFILE

#undef open
#define open(...) rf_open(__VA_ARGS__)
#undef read
#define read(fd, buf, count) rf_read(fd, buf, count)
#undef write
#define write(fd, buf, count) rf_write(fd, buf, count)
#undef lseek
#define lseek(fd, offset, wh) rf_lseek(fd, offset, wh)
#undef close
#define close(fd) rf_close(fd)
#undef unlink
#define unlink(path) rf_unlink(path)
#undef remove
#define remove(path) rf_remove(path)
#undef rename
#define rename(old, new) rf_rename(old, new)
#undef access
#define access(path, mode) rf_access(path, mode)
#undef isatty
#define isatty(fd) rf_isatty(fd)
#undef stat
#define stat(path, buf) rf_stat(path, buf)
#undef fstat
#define fstat(fd, buf) rf_fstat(fd, buf)

#undef fopen
#define fopen(path, mode) rf_fopen(path, mode)
#undef fread
#define fread(ptr, sz, n, st) rf_fread(ptr, sz, n, st)
#undef fwrite
#define fwrite(ptr, sz, n, st) rf_fwrite(ptr, sz, n, st)
#undef fseek
#define fseek(st, off, wh) rf_fseek(st, off, wh)
#undef ftell
#define ftell(st) rf_ftell(st)
#undef rewind
#define rewind(st) rf_rewind(st)
#undef fflush
#define fflush(st) rf_fflush(st)
#undef feof
#define feof(st) rf_feof(st)
#undef ferror
#define ferror(st) rf_ferror(st)
#undef clearerr
#define clearerr(st) rf_clearerr(st)
#undef fileno
#define fileno(st) rf_fileno(st)
#undef fgetc
#define fgetc(st) rf_fgetc(st)
#undef getc
#define getc(st) rf_fgetc(st)
#undef ungetc
#define ungetc(c, st) rf_ungetc(c, st)
#undef fgets
#define fgets(buf, sz, st) rf_fgets(buf, sz, st)
#undef fputc
#define fputc(c, st) rf_fputc(c, st)
#undef putc
#define putc(c, st) rf_fputc(c, st)
#undef fputs
#define fputs(s, st) rf_fputs(s, st)
#undef fprintf
#define fprintf(...) rf_fprintf(__VA_ARGS__)
#undef fscanf
#define fscanf(...) rf_fscanf(__VA_ARGS__)
#undef fclose
#define fclose(st) rf_fclose(st)

#endif
