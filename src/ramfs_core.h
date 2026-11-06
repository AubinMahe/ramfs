#pragma once

#include "ramfs.h"
#include "ramfs_sync.h"
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#define RAMFS_INLINE_CAPACITY 1024
#define RAMFS_CHUNK_SHIFT 16
#define RAMFS_CHUNK_SIZE (1UL << RAMFS_CHUNK_SHIFT) /* 65536 octets = 64 Ko */
#define RAMFS_CHUNK_MASK (RAMFS_CHUNK_SIZE - 1UL)
#define RAMFS_SLAB_CHUNKS 16 /* 16 blocs de 64 Ko = 1 Mo par tranche (Slab) */
#define MIN_TABLE_SIZE 8

/* =========================================================================
 * Types du catalogue et de l'espace mémoire
 * ========================================================================= */

typedef struct {
   unsigned char inline_data[RAMFS_INLINE_CAPACITY]; /* Données inlinées pour fichiers <= 1 Ko (SFO) */
   unsigned char *first_chunk;                       /* Premier bloc paginé de 64 Ko */
   unsigned char **chunks;                           /* Tableau de pointeurs alloué si chunks_count > 1 */
   size_t chunks_count;                              /* Nombre de blocs paginés alloués */
   size_t chunks_capacity;                           /* Capacité du tableau de pointeurs chunks */
   size_t size;                                      /* Taille logique du fichier en octets */
   int is_dir;                                       /* 1 si répertoire, 0 si fichier régulier */
   int is_inlined;                                   /* 1 si contenu dans inline_data, 0 si paginé */
   rfs_rwlock_t *rwlock;                             /* Verrou lecteur/rédacteur par fichier */
} ram_file_t;

/* Entrée de la table de hachage. path == NULL indique une case libre. */
typedef struct {
   char *path;
   ram_file_t *file;
   size_t hash;
} ram_catalog_t;

typedef struct {
   off_t offset;
   unsigned ref_count;
   rfs_mutex_t lock;
} ram_shared_offset_t;

typedef struct {
   ram_file_t *file;
   ram_shared_offset_t *shared_offset;
   int flags;
   int eof;
   int error;
   int is_tmpfile;
} ram_file_access_t;

/* =========================================================================
 * Variables globales du noyau (Core)
 * ========================================================================= */

extern ram_file_t *g_files;
extern unsigned *g_free_stack;
extern unsigned g_free_top;
extern unsigned g_files_capacity;

extern ram_catalog_t *g_table;
extern size_t g_table_mask;

extern ram_file_access_t *g_access_table;
extern unsigned g_access_capacity;

extern unsigned g_files_used;
extern unsigned g_peak_files;
extern unsigned g_access_used;
extern unsigned g_peak_access;

/* =========================================================================
 * Fonctions de gestion du catalogue et d'accès
 * ========================================================================= */

size_t hash_path(const char *s);
ram_catalog_t *search_in_catalog(const char *path, size_t hash);
ram_catalog_t *create_entry_in_catalog(const char *path, size_t hash);
ram_catalog_t *create_dir_entry_in_catalog(const char *path, size_t hash);
int is_dir_prefix(const char *clean_path);
void catalog_depth_add(const char *path);
void remove_from_catalog(size_t i);
ram_file_access_t *get_access(rfs_handle_t fd);
int resolve_path(const char *src, char *dst, size_t dst_size);
void fill_stat_buf(const ram_file_t *file, struct stat *buf);
int expand_access_table(void);

/* Opérations de stockage paginé (Chunked Storage) */
unsigned char *rfs_file_get_chunk(const ram_file_t *file, size_t idx);
size_t rfs_file_read_bytes(const ram_file_t *file, size_t offset, void *buf, size_t count);
int rfs_file_write_bytes(ram_file_t *file, size_t offset, const void *buf, size_t count);
int rfs_file_truncate(ram_file_t *file, size_t new_len);
void rfs_file_free_storage(ram_file_t *file);
int rfs_file_get_byte(const ram_file_t *file, size_t offset);
int rfs_file_put_byte(ram_file_t *file, size_t offset, unsigned char c);

/* Synchronisation et verrous (Thread-Safety) */
void rfs_file_lock_init(ram_file_t *file);
void rfs_file_lock_destroy(ram_file_t *file);
void rfs_file_rdlock(ram_file_t *file);
void rfs_file_wrlock(ram_file_t *file);
void rfs_file_unlock(ram_file_t *file);

void rfs_catalog_rdlock(void);
void rfs_catalog_wrlock(void);
void rfs_catalog_unlock(void);

void rfs_access_lock(void);
void rfs_access_unlock(void);
