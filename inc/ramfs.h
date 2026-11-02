#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef BUILDING_RAMFS_DLL
#define RAMFS_API __declspec(dllexport)
#else
#define RAMFS_API __declspec(dllimport)
#endif
#else
#define RAMFS_API __attribute__((visibility("default")))
#endif

/* Rétro-compatibilité pour l'ancien nom de macro d'export */
#define RAMFILE_API RAMFS_API

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef int rfs_handle_t;
#define RFS_INVALID_HANDLE (-1)

/* Alias rétro-compatibles */
typedef rfs_handle_t rf_handle_t;
#define RF_INVALID_HANDLE RFS_INVALID_HANDLE

typedef struct {
   unsigned files_used;      /* Nombre actuel de fichiers en mémoire */
   unsigned peak_files;      /* Pic maximal de fichiers simultanés */
   unsigned files_capacity;  /* Capacité configurée lors de rfs_init */
   unsigned access_used;     /* Nombre actuel de descripteurs ouverts */
   unsigned peak_access;     /* Pic maximal de descripteurs simultanés */
   unsigned access_capacity; /* Capacité configurée lors de rfs_init */
} rfs_stats_t;

typedef rfs_stats_t rf_stats_t;

/**
 * Initialise les structures de données de ramfs à partir des limites fournies.
 *
 * @param file_count nombre maximal de fichiers
 * @param access_count nombre maximal de descripteur de fichier (open, fopen)
 * @return 0 en cas de succès, -1 en cas d'erreur (errno positionné).
 */
RAMFS_API int rfs_init(unsigned file_count, unsigned access_count);

/**
 * Libère les ressources, réinitialise les descripteurs statiques.
 */
RAMFS_API void rfs_destroy(void);

/**
 * Récupère les métriques d'utilisation et de crête de la bibliothèque.
 *
 * @param stats pointeur vers la structure recevant les métriques
 * @return 0 en cas de succès, -1 si non initialisé (errno = ENODEV) ou stats NULL (errno = EINVAL).
 */
RAMFS_API int rfs_get_stats(rfs_stats_t *stats);

/**
 * Affiche un rapport synthétique des jauges et pics d'utilisation.
 *
 * @param stream flux cible (si NULL, affiche sur stderr).
 */
RAMFS_API void rfs_dump_stats(FILE *stream);

RAMFS_API rfs_handle_t rfs_open(const char *pathname, int flags, ...);
RAMFS_API ssize_t rfs_read(rfs_handle_t fd, void *buf, size_t count);
RAMFS_API ssize_t rfs_write(rfs_handle_t fd, const void *buf, size_t count);
RAMFS_API off_t rfs_lseek(rfs_handle_t fd, off_t offset, int whence);
RAMFS_API int rfs_close(rfs_handle_t fd);
RAMFS_API int rfs_unlink(const char *pathname);
RAMFS_API int rfs_remove(const char *pathname);
RAMFS_API int rfs_stat(const char *pathname, struct stat *buf);
RAMFS_API int rfs_fstat(rfs_handle_t fd, struct stat *buf);
RAMFS_API int rfs_rename(const char *oldpath, const char *newpath);
RAMFS_API int rfs_access(const char *pathname, int mode);
RAMFS_API int rfs_mkdir(const char *pathname, ...);
RAMFS_API int rfs_rmdir(const char *pathname);
RAMFS_API int rfs_isatty(rfs_handle_t fd);
RAMFS_API int rfs_normalize_path(const char *src, char *dst, size_t dst_size);

RAMFS_API FILE *rfs_fopen(const char *pathname, const char *mode);
RAMFS_API size_t rfs_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
RAMFS_API size_t rfs_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
RAMFS_API int rfs_fseek(FILE *stream, long offset, int whence);
RAMFS_API long rfs_ftell(FILE *stream);
RAMFS_API void rfs_rewind(FILE *stream);
RAMFS_API int rfs_fflush(FILE *stream);
RAMFS_API int rfs_feof(FILE *stream);
RAMFS_API int rfs_ferror(FILE *stream);
RAMFS_API void rfs_clearerr(FILE *stream);
RAMFS_API int rfs_fileno(FILE *stream);
RAMFS_API int rfs_fgetc(FILE *stream);
RAMFS_API int rfs_ungetc(int c, FILE *stream);
RAMFS_API char *rfs_fgets(char *s, int size, FILE *stream);
RAMFS_API int rfs_fputc(int c, FILE *stream);
RAMFS_API int rfs_fputs(const char *s, FILE *stream);
RAMFS_API int rfs_fprintf(FILE *stream, const char *format, ...);
RAMFS_API int rfs_vfprintf(FILE *stream, const char *format, va_list ap);
RAMFS_API int rfs_fscanf(FILE *stream, const char *format, ...);
RAMFS_API int rfs_fclose(FILE *stream);

/**
 * Sauvegarde une sélection de fichiers en mémoire vers une archive compressée (.tar.gz) ou vers le disque.
 *
 * @param paths tableau de chemins à sauvegarder (si NULL, tous les fichiers en mémoire sont sauvegardés).
 * @param path_count nombre d'éléments dans paths (si 0 et paths != NULL, le tableau est considéré terminé par NULL).
 * @param archive_path si non NULL, crée une archive compressée .tar.gz contenant les fichiers sélectionnés.
 * @param dest_dir si archive_path == NULL, répertoire racine de destination sur disque (si NULL, relatif au PWD).
 * @return 0 en cas de succès, -1 en cas d'erreur (errno positionné).
 */
RAMFS_API int rfs_save(const char *const *paths, size_t path_count, const char *archive_path, const char *dest_dir);

/**
 * Réciproque de rfs_save : charge et peuple la mémoire de ramfs depuis une archive (.tar.gz) ou le disque.
 *
 * @param archive_path si non NULL, chemin de l'archive compressée .tar.gz à charger en mémoire.
 * @param src_dir si archive_path == NULL, répertoire racine source sur disque à charger (si NULL, relatif au PWD).
 * @param prefix préfixe optionnel à ajouter aux chemins extraits en mémoire (NULL pour conserver l'arborescence telle quelle).
 * @return le nombre de fichiers chargés en mémoire en cas de succès (>= 0), ou -1 en cas d'erreur (errno positionné).
 */
RAMFS_API int rfs_load(const char *archive_path, const char *src_dir, const char *prefix);

/**
 * Peuple la mémoire de ramfs en lisant récursivement un dossier sur disque.
 * Chaque fichier est lu et recréé fidèlement en RAM.
 *
 * @param dir_path chemin du répertoire racine sur disque à charger (si NULL, dossier courant ".").
 * @param prefix préfixe optionnel sous lequel enregistrer les fichiers en mémoire (NULL pour conserver l'arborescence relative).
 * @return le nombre de fichiers chargés en mémoire en cas de succès (>= 0), ou -1 en cas d'erreur (errno positionné).
 */
RAMFS_API int rfs_load_dir(const char *dir_path, const char *prefix);

/* Alias rétro-compatibles pour l'API C */
#define rf_init rfs_init
#define rf_destroy rfs_destroy
#define rf_get_stats rfs_get_stats
#define rf_dump_stats rfs_dump_stats
#define rf_open rfs_open
#define rf_read rfs_read
#define rf_write rfs_write
#define rf_lseek rfs_lseek
#define rf_close rfs_close
#define rf_unlink rfs_unlink
#define rf_remove rfs_remove
#define rf_stat rfs_stat
#define rf_fstat rfs_fstat
#define rf_rename rfs_rename
#define rf_access rfs_access
#define rf_mkdir rfs_mkdir
#define rf_rmdir rfs_rmdir
#define rf_isatty rfs_isatty
#define rf_normalize_path rfs_normalize_path
#define rf_fopen rfs_fopen
#define rf_fread rfs_fread
#define rf_fwrite rfs_fwrite
#define rf_fseek rfs_fseek
#define rf_ftell rfs_ftell
#define rf_rewind rfs_rewind
#define rf_fflush rfs_fflush
#define rf_feof rfs_feof
#define rf_ferror rfs_ferror
#define rf_clearerr rfs_clearerr
#define rf_fileno rfs_fileno
#define rf_fgetc rfs_fgetc
#define rf_ungetc rfs_ungetc
#define rf_fgets rfs_fgets
#define rf_fputc rfs_fputc
#define rf_fputs rfs_fputs
#define rf_fprintf rfs_fprintf
#define rf_vfprintf rfs_vfprintf
#define rf_fscanf rfs_fscanf
#define rf_fclose rfs_fclose
#define rf_save rfs_save
#define rf_load rfs_load
#define rf_load_dir rfs_load_dir

#if defined(USE_RAMFS) || defined(USE_RAMFILE)

#undef open
#define open(...) rfs_open(__VA_ARGS__)
#undef read
#define read(fd, buf, count) rfs_read(fd, buf, count)
#undef write
#define write(fd, buf, count) rfs_write(fd, buf, count)
#undef lseek
#define lseek(fd, offset, wh) rfs_lseek(fd, offset, wh)
#undef close
#define close(fd) rfs_close(fd)
#undef unlink
#define unlink(path) rfs_unlink(path)
#undef remove
#define remove(path) rfs_remove(path)
#undef rename
#define rename(old, new) rfs_rename(old, new)
#undef access
#define access(path, mode) rfs_access(path, mode)
#undef mkdir
#define mkdir(...) rfs_mkdir(__VA_ARGS__)
#undef _mkdir
#define _mkdir(path) rfs_mkdir(path)
#undef rmdir
#define rmdir(path) rfs_rmdir(path)
#undef _rmdir
#define _rmdir(path) rfs_rmdir(path)
#undef isatty
#define isatty(fd) rfs_isatty(fd)
#undef stat
#define stat(path, buf) rfs_stat(path, buf)
#undef fstat
#define fstat(fd, buf) rfs_fstat(fd, buf)

#undef fopen
#define fopen(path, mode) rfs_fopen(path, mode)
#undef fread
#define fread(ptr, sz, n, st) rfs_fread(ptr, sz, n, st)
#undef fwrite
#define fwrite(ptr, sz, n, st) rfs_fwrite(ptr, sz, n, st)
#undef fseek
#define fseek(st, off, wh) rfs_fseek(st, off, wh)
#undef ftell
#define ftell(st) rfs_ftell(st)
#undef rewind
#define rewind(st) rfs_rewind(st)
#undef fflush
#define fflush(st) rfs_fflush(st)
#undef feof
#define feof(st) rfs_feof(st)
#undef ferror
#define ferror(st) rfs_ferror(st)
#undef clearerr
#define clearerr(st) rfs_clearerr(st)
#undef fileno
#define fileno(st) rfs_fileno(st)
#undef fgetc
#define fgetc(st) rfs_fgetc(st)
#undef getc
#define getc(st) rfs_fgetc(st)
#undef ungetc
#define ungetc(c, st) rfs_ungetc(c, st)
#undef fgets
#define fgets(buf, sz, st) rfs_fgets(buf, sz, st)
#undef fputc
#define fputc(c, st) rfs_fputc(c, st)
#undef putc
#define putc(c, st) rfs_fputc(c, st)
#undef fputs
#define fputs(s, st) rfs_fputs(s, st)
#undef fprintf
#define fprintf(...) rfs_fprintf(__VA_ARGS__)
#undef vfprintf
#define vfprintf(st, fmt, ap) rfs_vfprintf(st, fmt, ap)
#undef fscanf
#define fscanf(...) rfs_fscanf(__VA_ARGS__)
#undef fclose
#define fclose(st) rfs_fclose(st)

#endif
