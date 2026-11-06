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

#include <dirent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef int rfs_handle_t;
#define RFS_INVALID_HANDLE (-1)

typedef struct {
   unsigned files_used;      /* Nombre actuel de fichiers en mémoire */
   unsigned peak_files;      /* Pic maximal de fichiers simultanés */
   unsigned files_capacity;  /* Capacité configurée lors de rfs_init */
   unsigned access_used;     /* Nombre actuel de descripteurs ouverts */
   unsigned peak_access;     /* Pic maximal de descripteurs simultanés */
   unsigned access_capacity; /* Capacité configurée lors de rfs_init */
} rfs_stats_t;

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
 * Configure un point de montage virtuel pour le mode VFS Hybride.
 * Tout chemin débutant par ce préfixe est traité en RAM par ramfs.
 * Tout chemin en dehors de ce préfixe est relayé de façon transparente vers le système de fichiers réel (libc).
 *
 * Si prefix est NULL ou "", le filtrage est désactivé et tous les chemins sont gérés en RAM.
 *
 * @param prefix chemin du point de montage (ex: "/ramfs", "ramfs", "./tmp/ram")
 * @return 0 en cas de succès, -1 en cas d'erreur.
 */
RAMFS_API int rfs_mount(const char *prefix);

/**
 * Retourne le point de montage virtuel actuel, ou NULL si non configuré.
 */
RAMFS_API const char *rfs_get_mount(void);

/**
 * Démonte le point de montage virtuel (désactive le filtrage hybride).
 */
RAMFS_API void rfs_unmount(void);

/**
 * Retourne la version de la bibliothèque fixée par le tag git le plus récent.
 *
 * @return chaîne de caractères constante contenant le numéro de version (ex: "v1.0.0").
 */
RAMFS_API const char *rfs_version(void);

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
RAMFS_API int rfs_dup(rfs_handle_t oldfd);
RAMFS_API int rfs_dup2(rfs_handle_t oldfd, rfs_handle_t newfd);
RAMFS_API int rfs_ftruncate(rfs_handle_t fd, off_t length);
RAMFS_API int rfs_truncate(const char *pathname, off_t length);

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif

struct rfs_dirent {
   ino_t d_ino;
   unsigned char d_type;
   char d_name[256];
};

typedef struct rfs_dir rfs_dir_t;

RAMFS_API rfs_dir_t *rfs_opendir(const char *name);
RAMFS_API struct rfs_dirent *rfs_readdir(rfs_dir_t *dirp);
RAMFS_API int rfs_closedir(rfs_dir_t *dirp);
RAMFS_API void rfs_rewinddir(rfs_dir_t *dirp);
RAMFS_API long rfs_telldir(rfs_dir_t *dirp);
RAMFS_API void rfs_seekdir(rfs_dir_t *dirp, long loc);

RAMFS_API int rfs_chdir(const char *path);
RAMFS_API char *rfs_getcwd(char *buf, size_t size);

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
RAMFS_API FILE *rfs_tmpfile(void);
RAMFS_API ssize_t rfs_getdelim(char **lineptr, size_t *n, int delimiter, FILE *stream);
RAMFS_API ssize_t rfs_getline(char **lineptr, size_t *n, FILE *stream);

/**
 * Sauvegarde une sélection ou la totalité des fichiers en mémoire vers une archive compressée (.tar.gz).
 *
 * @param archive_path chemin de l'archive .tar.gz à créer (ne doit pas être NULL).
 * @param paths tableau optionnel de chemins à sauvegarder (si NULL, tous les fichiers sont archivés).
 * @param path_count nombre d'éléments dans paths (si 0 et paths != NULL, le tableau est considéré terminé par NULL).
 * @return 0 en cas de succès, -1 en cas d'erreur (errno positionné).
 */
RAMFS_API int rfs_save_archive(const char *archive_path, const char *const *paths, size_t path_count);

/**
 * Sauvegarde une sélection ou la totalité des fichiers en mémoire en recréant l'arborescence sur le disque.
 *
 * @param dest_dir répertoire racine de destination sur disque (si NULL, dossier courant ".").
 * @param paths tableau optionnel de chemins à sauvegarder (si NULL, tous les fichiers sont exportés).
 * @param path_count nombre d'éléments dans paths (si 0 et paths != NULL, le tableau est considéré terminé par NULL).
 * @return 0 en cas de succès, -1 en cas d'erreur (errno positionné).
 */
RAMFS_API int rfs_save_dir(const char *dest_dir, const char *const *paths, size_t path_count);

/**
 * Charge et peuple la mémoire de ramfs depuis une archive (.tar.gz) ou le disque.
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

#ifdef USE_RAMFS

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
#undef dup
#define dup(fd) rfs_dup(fd)
#undef dup2
#define dup2(oldfd, newfd) rfs_dup2(oldfd, newfd)
#undef ftruncate
#define ftruncate(fd, length) rfs_ftruncate(fd, length)
#undef truncate
#define truncate(path, length) rfs_truncate(path, length)

#undef chdir
#define chdir(path) rfs_chdir(path)
#undef _chdir
#define _chdir(path) rfs_chdir(path)
#undef getcwd
#define getcwd(buf, sz) rfs_getcwd(buf, sz)
#undef _getcwd
#define _getcwd(buf, sz) rfs_getcwd(buf, sz)

#undef DIR
#define DIR rfs_dir_t
#undef dirent
#define dirent rfs_dirent
#undef opendir
#define opendir(name) rfs_opendir(name)
#undef readdir
#define readdir(dirp) rfs_readdir(dirp)
#undef closedir
#define closedir(dirp) rfs_closedir(dirp)
#undef rewinddir
#define rewinddir(dirp) rfs_rewinddir(dirp)
#undef telldir
#define telldir(dirp) rfs_telldir(dirp)
#undef seekdir
#define seekdir(dirp, loc) rfs_seekdir(dirp, loc)

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
#undef tmpfile
#define tmpfile() rfs_tmpfile()
#undef getline
#define getline(lineptr, n, st) rfs_getline(lineptr, n, st)
#undef getdelim
#define getdelim(lineptr, n, delim, st) rfs_getdelim(lineptr, n, delim, st)

#endif
