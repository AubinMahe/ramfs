#pragma once

#include "ramfs.h"
#include <limits.h>
#include <stddef.h>

#define RFS_FD_BASE 0x40000000

/* =========================================================================
 * VFS global state variables
 * ========================================================================= */

extern char g_cwd[PATH_MAX];
extern char g_mount_prefix[PATH_MAX];
extern size_t g_mount_len;
extern unsigned g_access_capacity;

/* =========================================================================
 * Descriptor conversion and filtering inlines
 * ========================================================================= */

static inline const char *skip_slashes(const char *p) {
   while (*p == '/' || *p == '\\') {
      p++;
   }
   return p;
}

static inline int is_rfs_fd(rfs_handle_t fd) {
   if (fd >= (rfs_handle_t)RFS_FD_BASE && (unsigned)(fd - RFS_FD_BASE) < g_access_capacity) {
      return 1;
   }
   if (g_mount_len == 0 && fd >= 0 && (unsigned)fd < g_access_capacity) {
      return 1;
   }
   return 0;
}

static inline unsigned fd_to_slot(rfs_handle_t fd) {
   if (fd >= (rfs_handle_t)RFS_FD_BASE) {
      return (unsigned)(fd - RFS_FD_BASE);
   }
   return (unsigned)fd;
}

static inline rfs_handle_t slot_to_fd(unsigned slot) {
   return g_mount_len > 0 ? (rfs_handle_t)(RFS_FD_BASE + slot) : (rfs_handle_t)slot;
}

/* =========================================================================
 * Mount point routing functions
 * ========================================================================= */

int is_under_mount(const char *clean_path, char *ram_rel, size_t rel_size);
