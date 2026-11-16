#ifndef FFAMP_H264_THREADS_H
#define FFAMP_H264_THREADS_H

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK FFMutex;
typedef CONDITION_VARIABLE FFCond;
typedef HANDLE FFThread;
static inline void ff_mutex_init(FFMutex *m) { InitializeSRWLock(m); }
static inline void ff_mutex_destroy(FFMutex *m) { (void)m; }
static inline void ff_mutex_lock(FFMutex *m) { AcquireSRWLockExclusive(m); }
static inline void ff_mutex_unlock(FFMutex *m) { ReleaseSRWLockExclusive(m); }
static inline void ff_cond_init(FFCond *c) { InitializeConditionVariable(c); }
static inline void ff_cond_destroy(FFCond *c) { (void)c; }
static inline void ff_cond_wait(FFCond *c, FFMutex *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static inline void ff_cond_signal(FFCond *c) { WakeConditionVariable(c); }
static inline void ff_cond_broadcast(FFCond *c) { WakeAllConditionVariable(c); }
#else
#include <pthread.h>
typedef pthread_mutex_t FFMutex;
typedef pthread_cond_t FFCond;
typedef pthread_t FFThread;
static inline void ff_mutex_init(FFMutex *m) { pthread_mutex_init(m, NULL); }
static inline void ff_mutex_destroy(FFMutex *m) { pthread_mutex_destroy(m); }
static inline void ff_mutex_lock(FFMutex *m) { pthread_mutex_lock(m); }
static inline void ff_mutex_unlock(FFMutex *m) { pthread_mutex_unlock(m); }
static inline void ff_cond_init(FFCond *c) { pthread_cond_init(c, NULL); }
static inline void ff_cond_destroy(FFCond *c) { pthread_cond_destroy(c); }
static inline void ff_cond_wait(FFCond *c, FFMutex *m) { pthread_cond_wait(c, m); }
static inline void ff_cond_signal(FFCond *c) { pthread_cond_signal(c); }
static inline void ff_cond_broadcast(FFCond *c) { pthread_cond_broadcast(c); }
#endif

int ff_thread_start(FFThread *thread, void *(*run)(void *), void *arg);
void ff_thread_join(FFThread thread);
int ff_cpu_count(void);

#endif
