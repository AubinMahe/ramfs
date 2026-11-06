#include "ramfs_vfs.h"
#include "ramfs_core.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

/* =========================================================================
 * Variables globales du VFS
 * ========================================================================= */

/* Répertoire de travail virtuel */
char g_cwd[PATH_MAX] = "/";

/* Point de montage virtuel (Mode VFS Hybride) */
char g_mount_prefix[PATH_MAX] = "";
size_t g_mount_len = 0;

/* =========================================================================
 * Gestion du point de montage (rfs_mount / rfs_unmount)
 * ========================================================================= */

void rfs_unmount(void) {
   g_mount_prefix[0] = '\0';
   g_mount_len = 0;
}

int rfs_mount(const char *prefix) {
   if (!prefix || prefix[0] == '\0') {
      rfs_unmount();
      return 0;
   }
   char norm[PATH_MAX];
   if (rfs_normalize_path(prefix, norm, sizeof(norm)) != 0) {
      return -1;
   }
   size_t len = strlen(norm);
   while (len > 1 && norm[len - 1] == '/') {
      norm[--len] = '\0';
   }
   snprintf(g_mount_prefix, sizeof(g_mount_prefix), "%s", norm);
   g_mount_len = strlen(g_mount_prefix);
   return 0;
}

const char *rfs_get_mount(void) { return g_mount_len > 0 ? g_mount_prefix : NULL; }

int is_under_mount(const char *clean_path, char *ram_rel, size_t rel_size) {
   if (g_mount_len == 0) {
      if (ram_rel && rel_size > 0) {
         snprintf(ram_rel, rel_size, "%s", clean_path ? clean_path : "");
      }
      return 1;
   }
   if (!clean_path) {
      return 0;
   }
   const char *p = skip_slashes(clean_path);
   const char *m = skip_slashes(g_mount_prefix);
   size_t m_len = strlen(m);
   if (m_len == 0) {
      if (ram_rel && rel_size > 0) {
         snprintf(ram_rel, rel_size, "%s", clean_path);
      }
      return 1;
   }
   if (strncmp(p, m, m_len) == 0 && (p[m_len] == '\0' || p[m_len] == '/' || p[m_len] == '\\')) {
      if (ram_rel && rel_size > 0) {
         const char *sub = skip_slashes(p + m_len);
         if (*sub == '\0') {
            snprintf(ram_rel, rel_size, ".");
         } else {
            snprintf(ram_rel, rel_size, "%s", sub);
         }
      }
      return 1;
   }
   return 0;
}

/* =========================================================================
 * Répertoire courant virtuel (chdir / getcwd)
 * ========================================================================= */

int rfs_chdir(const char *path) {
   if (!path) {
      errno = EINVAL;
      return -1;
   }
   char target[PATH_MAX];
   if (resolve_path(path, target, sizeof(target)) != 0) {
      return -1;
   }
   char ram_rel[PATH_MAX];
   if (!is_under_mount(target, ram_rel, sizeof(ram_rel))) {
#undef chdir
      int ret = chdir(path);
      if (ret == 0) {
         snprintf(g_cwd, sizeof(g_cwd), "%s", target);
      }
      return ret;
   }
   if (!g_files || !g_table) {
      errno = ENODEV;
      return -1;
   }
   if (ram_rel[0] == '\0' || strcmp(ram_rel, "/") == 0 || strcmp(ram_rel, ".") == 0) {
      if (g_mount_len > 0) {
         snprintf(g_cwd, sizeof(g_cwd), "%s", g_mount_prefix);
      } else {
         snprintf(g_cwd, sizeof(g_cwd), "/");
      }
      return 0;
   }
   ram_catalog_t *entry = search_in_catalog(ram_rel, hash_path(ram_rel));
   if (entry) {
      if (!entry->file->is_dir) {
         errno = ENOTDIR;
         return -1;
      }
   } else if (!is_dir_prefix(ram_rel)) {
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
