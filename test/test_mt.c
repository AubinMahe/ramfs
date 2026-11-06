#include "ramfs.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NUM_WORKERS 16
#define OPS_PER_WORKER 500
#define NUM_SHARED_FILES 8
#define BLOCK_SIZE 64

static int g_mt_failures = 0;
static pthread_mutex_t g_fail_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MT_CHECK(cond)                                                                                                             \
   do {                                                                                                                            \
      if (!(cond)) {                                                                                                               \
         pthread_mutex_lock(&g_fail_mutex);                                                                                        \
         g_mt_failures++;                                                                                                          \
         fprintf(stderr, "[THREAD FAIL] %s:%d: %s (errno=%d: %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno));           \
         pthread_mutex_unlock(&g_fail_mutex);                                                                                      \
      }                                                                                                                            \
   } while (0)

typedef struct {
   int thread_id;
} worker_arg_t;

/* Shared files initialization */
static void setup_shared_files(void) {
   char path[64];
   char data[128];
   for (int i = 0; i < NUM_SHARED_FILES; i++) {
      /* Shared read-only files */
      snprintf(path, sizeof(path), "shared_ro/data_%d.dat", i);
      snprintf(data, sizeof(data), "SHARED_DATA_CONTENT_PATTERN_%04d", i);
      rfs_handle_t fd = rfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
      MT_CHECK(fd >= 0);
      if (fd >= 0) {
         ssize_t w = rfs_write(fd, data, strlen(data));
         MT_CHECK(w == (ssize_t)strlen(data));
         MT_CHECK(rfs_close(fd) == 0);
      }

      /* Shared atomic read/write files */
      snprintf(path, sizeof(path), "shared_rw/data_%d.dat", i);
      char block[BLOCK_SIZE];
      memset(block, 'Z', sizeof(block));
      fd = rfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
      MT_CHECK(fd >= 0);
      if (fd >= 0) {
         ssize_t w = rfs_write(fd, block, sizeof(block));
         MT_CHECK(w == (ssize_t)sizeof(block));
         MT_CHECK(rfs_close(fd) == 0);
      }
   }
}

/* Group 1 (threads 0-3): private creates, reads and deletions */
static void worker_create_delete(int tid) {
   char path[64];
   char wbuf[64];
   char rbuf[64];
   for (int i = 0; i < OPS_PER_WORKER; i++) {
      snprintf(path, sizeof(path), "th_%d/f_%04d.dat", tid, i);
      snprintf(wbuf, sizeof(wbuf), "PAYLOAD_TID%d_OP%04d", tid, i);
      size_t len = strlen(wbuf);

      rfs_handle_t fd = rfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
      MT_CHECK(fd >= 0);
      if (fd >= 0) {
         MT_CHECK(rfs_write(fd, wbuf, len) == (ssize_t)len);
         MT_CHECK(rfs_close(fd) == 0);
      }

      fd = rfs_open(path, O_RDONLY);
      MT_CHECK(fd >= 0);
      if (fd >= 0) {
         memset(rbuf, 0, sizeof(rbuf));
         MT_CHECK(rfs_read(fd, rbuf, len) == (ssize_t)len);
         MT_CHECK(memcmp(wbuf, rbuf, len) == 0);
         MT_CHECK(rfs_close(fd) == 0);
      }

      MT_CHECK(rfs_unlink(path) == 0);
   }
}

/* Group 2 (threads 4-7): concurrent shared readers */
static void worker_shared_read(int tid) {
   (void)tid;
   char path[64];
   char expected[128];
   char rbuf[128];
   for (int i = 0; i < OPS_PER_WORKER; i++) {
      int idx = i % NUM_SHARED_FILES;
      snprintf(path, sizeof(path), "shared_ro/data_%d.dat", idx);
      snprintf(expected, sizeof(expected), "SHARED_DATA_CONTENT_PATTERN_%04d", idx);
      size_t len = strlen(expected);

      rfs_handle_t fd = rfs_open(path, O_RDONLY);
      MT_CHECK(fd >= 0);
      if (fd >= 0) {
         memset(rbuf, 0, sizeof(rbuf));
         MT_CHECK(rfs_read(fd, rbuf, len) == (ssize_t)len);
         MT_CHECK(memcmp(expected, rbuf, len) == 0);
         MT_CHECK(rfs_close(fd) == 0);
      }
   }
}

/* Group 3 (threads 8-11): concurrent atomic updates and reads */
static void worker_shared_update(int tid) {
   char path[64];
   char wblock[BLOCK_SIZE];
   char rblock[BLOCK_SIZE];
   char val = (char)('A' + tid);
   memset(wblock, val, sizeof(wblock));

   for (int i = 0; i < OPS_PER_WORKER; i++) {
      int idx = i % NUM_SHARED_FILES;
      snprintf(path, sizeof(path), "shared_rw/data_%d.dat", idx);

      /* Atomic write under wrlock (yield if busy) */
      rfs_handle_t fd = RFS_INVALID_HANDLE;
      while ((fd = rfs_open(path, O_WRONLY)) < 0) {
         if (errno == EBUSY) {
            sched_yield();
            continue;
         }
         break;
      }
      MT_CHECK(fd >= 0);
      if (fd >= 0) {
         MT_CHECK(rfs_lseek(fd, 0, SEEK_SET) == 0);
         MT_CHECK(rfs_write(fd, wblock, sizeof(wblock)) == (ssize_t)sizeof(wblock));
         MT_CHECK(rfs_close(fd) == 0);
      }

      /* Atomic read under rdlock (verifies no torn reads) */
      fd = RFS_INVALID_HANDLE;
      while ((fd = rfs_open(path, O_RDONLY)) < 0) {
         if (errno == EBUSY) {
            sched_yield();
            continue;
         }
         break;
      }
      MT_CHECK(fd >= 0);
      if (fd >= 0) {
         memset(rblock, 0, sizeof(rblock));
         MT_CHECK(rfs_read(fd, rblock, sizeof(rblock)) == (ssize_t)sizeof(rblock));
         char first = rblock[0];
         int torn = 0;
         for (size_t j = 1; j < sizeof(rblock); j++) {
            if (rblock[j] != first) {
               torn = 1;
               break;
            }
         }
         MT_CHECK(torn == 0);
         MT_CHECK(rfs_close(fd) == 0);
      }
   }
}

/* Group 4 (threads 12-15): concurrent stat & opendir inspection */
static void worker_inspect_dir(int tid) {
   (void)tid;
   struct stat st;
   for (int i = 0; i < OPS_PER_WORKER; i++) {
      int idx = i % NUM_SHARED_FILES;
      char path[64];
      snprintf(path, sizeof(path), "shared_ro/data_%d.dat", idx);
      MT_CHECK(rfs_stat(path, &st) == 0);
      MT_CHECK(st.st_size > 0);

      rfs_dir_t *d = rfs_opendir("shared_ro");
      MT_CHECK(d != NULL);
      if (d) {
         struct rfs_dirent *ent;
         size_t count = 0;
         while ((ent = rfs_readdir(d)) != NULL) {
            count++;
         }
         MT_CHECK(count >= 2); /* At least . and .. */
         MT_CHECK(rfs_closedir(d) == 0);
      }
   }
}

static void *worker_func(void *arg) {
   worker_arg_t *warg = (worker_arg_t *)arg;
   int tid = warg->thread_id;
   if (tid < 4) {
      worker_create_delete(tid);
   } else if (tid < 8) {
      worker_shared_read(tid);
   } else if (tid < 12) {
      worker_shared_update(tid);
   } else {
      worker_inspect_dir(tid);
   }
   return NULL;
}

int main(void) {
   printf("=== Multi-threaded stress test (%d workers, %d ops/thread) ===\n", NUM_WORKERS, OPS_PER_WORKER);
   MT_CHECK(rfs_init(0, 0) == 0);

   setup_shared_files();

   pthread_t threads[NUM_WORKERS];
   worker_arg_t args[NUM_WORKERS];

   for (int i = 0; i < NUM_WORKERS; i++) {
      args[i].thread_id = i;
      int rc = pthread_create(&threads[i], NULL, worker_func, &args[i]);
      MT_CHECK(rc == 0);
   }

   for (int i = 0; i < NUM_WORKERS; i++) {
      pthread_join(threads[i], NULL);
   }

   printf("=== Multi-threaded stress test finished: %s (Failures = %d) ===\n", g_mt_failures == 0 ? "SUCCESS" : "FAILURE",
          g_mt_failures);
   rfs_destroy();
   return g_mt_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
