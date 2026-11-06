#pragma once

#include "ramfs.h"
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#define RAMFS_INITIAL_CAPACITY 1024
#define MIN_TABLE_SIZE 8

/* =========================================================================
 * Types du catalogue et de l'espace mémoire
 * ========================================================================= */

typedef struct {
   unsigned char *data;
   size_t size;
   size_t capacity;
   int is_dir;
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
