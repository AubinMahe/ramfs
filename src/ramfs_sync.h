#pragma once

#if defined(RAMFS_THREAD_SAFE)

#include <pthread.h>
#include <stdlib.h>

typedef pthread_rwlock_t rfs_rwlock_t;
typedef pthread_mutex_t rfs_mutex_t;

#define RFS_RWLOCK_INIT(l) pthread_rwlock_init(l, NULL)
#define RFS_RWLOCK_DESTROY(l) pthread_rwlock_destroy(l)
#define RFS_RWLOCK_RDLOCK(l) pthread_rwlock_rdlock(l)
#define RFS_RWLOCK_WRLOCK(l) pthread_rwlock_wrlock(l)
#define RFS_RWLOCK_UNLOCK(l) pthread_rwlock_unlock(l)

#define RFS_MUTEX_INIT(m)                                                                                                          \
   do {                                                                                                                            \
      pthread_mutexattr_t _attr;                                                                                                   \
      pthread_mutexattr_init(&_attr);                                                                                              \
      pthread_mutexattr_settype(&_attr, PTHREAD_MUTEX_RECURSIVE);                                                                  \
      pthread_mutex_init((m), &_attr);                                                                                             \
      pthread_mutexattr_destroy(&_attr);                                                                                           \
   } while (0)
#define RFS_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#define RFS_MUTEX_LOCK(m) pthread_mutex_lock(m)
#define RFS_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)

static inline rfs_rwlock_t *rfs_rwlock_create(void) {
   rfs_rwlock_t *rw = (rfs_rwlock_t *)malloc(sizeof(rfs_rwlock_t));
   if (rw) {
      RFS_RWLOCK_INIT(rw);
   }
   return rw;
}

static inline void rfs_rwlock_free(rfs_rwlock_t *rw) {
   if (rw) {
      RFS_RWLOCK_DESTROY(rw);
      free(rw);
   }
}

#else

#include <stdlib.h>

typedef struct {
   int dummy;
} rfs_rwlock_t;

typedef struct {
   int dummy;
} rfs_mutex_t;

#define RFS_RWLOCK_INIT(l) ((void)(l))
#define RFS_RWLOCK_DESTROY(l) ((void)(l))
#define RFS_RWLOCK_RDLOCK(l) ((void)(l))
#define RFS_RWLOCK_WRLOCK(l) ((void)(l))
#define RFS_RWLOCK_UNLOCK(l) ((void)(l))

#define RFS_MUTEX_INIT(m) ((void)(m))
#define RFS_MUTEX_DESTROY(m) ((void)(m))
#define RFS_MUTEX_LOCK(m) ((void)(m))
#define RFS_MUTEX_UNLOCK(m) ((void)(m))

static inline rfs_rwlock_t *rfs_rwlock_create(void) {
   return NULL;
}

static inline void rfs_rwlock_free(rfs_rwlock_t *rw) {
   (void)rw;
}

#endif
