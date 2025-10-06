#ifndef PLATFORM_SYNC_H
#define PLATFORM_SYNC_H

#ifdef __MACOS__
// Classic Mac is single-threaded, so locks are no-ops.
typedef struct { int dummy; } platform_mutex_t;
#define platform_mutex_init(m) do {} while(0)
#define platform_mutex_lock(m) do {} while(0)
#define platform_mutex_unlock(m) do {} while(0)
#define platform_mutex_destroy(m) do {} while(0)
#else
// POSIX uses pthreads.
#include <pthread.h>
typedef pthread_mutex_t platform_mutex_t;
#define platform_mutex_init(m) pthread_mutex_init(m, NULL)
#define platform_mutex_lock(m) pthread_mutex_lock(m)
#define platform_mutex_unlock(m) pthread_mutex_unlock(m)
#define platform_mutex_destroy(m) pthread_mutex_destroy(m)
#endif

#endif // PLATFORM_SYNC_H
