#ifndef IVYTHREAD_H
#define IVYTHREAD_H

#ifndef IVY_TLS
#if defined(_MSC_VER)
#define IVY_TLS __declspec(thread)
#elif defined(__GNUC__)
#define IVY_TLS __thread
#else
#define IVY_TLS _Thread_local
#endif
#endif

#ifdef WIN32
#include <windows.h>

typedef CRITICAL_SECTION IvyMutex;
typedef SRWLOCK IvyRwLock;
typedef DWORD IvyThreadId;
typedef CONDITION_VARIABLE IvyCond;

static inline int IvyMutexInit(IvyMutex *mutex)
{
	InitializeCriticalSection(mutex);
	return 0;
}

static inline void IvyMutexDestroy(IvyMutex *mutex)
{
	DeleteCriticalSection(mutex);
}

static inline void IvyMutexLock(IvyMutex *mutex)
{
	EnterCriticalSection(mutex);
}

static inline void IvyMutexUnlock(IvyMutex *mutex)
{
	LeaveCriticalSection(mutex);
}

static inline int IvyRwLockInit(IvyRwLock *lock)
{
	InitializeSRWLock(lock);
	return 0;
}

static inline void IvyRwLockDestroy(IvyRwLock *lock)
{
	(void)lock;
}

static inline void IvyRwLockRdLock(IvyRwLock *lock)
{
	AcquireSRWLockShared(lock);
}

static inline void IvyRwLockWrLock(IvyRwLock *lock)
{
	AcquireSRWLockExclusive(lock);
}

static inline void IvyRwLockUnlockRead(IvyRwLock *lock)
{
	ReleaseSRWLockShared(lock);
}

static inline void IvyRwLockUnlockWrite(IvyRwLock *lock)
{
	ReleaseSRWLockExclusive(lock);
}

static inline IvyThreadId IvyThreadCurrent(void)
{
	return GetCurrentThreadId();
}

static inline int IvyThreadEqual(IvyThreadId a, IvyThreadId b)
{
	return a == b;
}

static inline int IvyCondInit(IvyCond *cond)
{
	InitializeConditionVariable(cond);
	return 0;
}

static inline void IvyCondDestroy(IvyCond *cond)
{
	(void)cond;
}

static inline void IvyCondWait(IvyCond *cond, IvyMutex *mutex)
{
	SleepConditionVariableCS(cond, mutex, INFINITE);
}

static inline void IvyCondBroadcast(IvyCond *cond)
{
	WakeAllConditionVariable(cond);
}

#else
#include <pthread.h>

typedef pthread_mutex_t IvyMutex;
typedef pthread_rwlock_t IvyRwLock;
typedef pthread_t IvyThreadId;
typedef pthread_cond_t IvyCond;

static inline int IvyMutexInit(IvyMutex *mutex)
{
	return pthread_mutex_init(mutex, NULL);
}

static inline void IvyMutexDestroy(IvyMutex *mutex)
{
	pthread_mutex_destroy(mutex);
}

static inline void IvyMutexLock(IvyMutex *mutex)
{
	pthread_mutex_lock(mutex);
}

static inline void IvyMutexUnlock(IvyMutex *mutex)
{
	pthread_mutex_unlock(mutex);
}

static inline int IvyRwLockInit(IvyRwLock *lock)
{
	return pthread_rwlock_init(lock, NULL);
}

static inline void IvyRwLockDestroy(IvyRwLock *lock)
{
	pthread_rwlock_destroy(lock);
}

static inline void IvyRwLockRdLock(IvyRwLock *lock)
{
	pthread_rwlock_rdlock(lock);
}

static inline void IvyRwLockWrLock(IvyRwLock *lock)
{
	pthread_rwlock_wrlock(lock);
}

static inline void IvyRwLockUnlockRead(IvyRwLock *lock)
{
	pthread_rwlock_unlock(lock);
}

static inline void IvyRwLockUnlockWrite(IvyRwLock *lock)
{
	pthread_rwlock_unlock(lock);
}

static inline IvyThreadId IvyThreadCurrent(void)
{
	return pthread_self();
}

static inline int IvyThreadEqual(IvyThreadId a, IvyThreadId b)
{
	return pthread_equal(a, b);
}

static inline int IvyCondInit(IvyCond *cond)
{
	return pthread_cond_init(cond, NULL);
}

static inline void IvyCondDestroy(IvyCond *cond)
{
	pthread_cond_destroy(cond);
}

static inline void IvyCondWait(IvyCond *cond, IvyMutex *mutex)
{
	pthread_cond_wait(cond, mutex);
}

static inline void IvyCondBroadcast(IvyCond *cond)
{
	pthread_cond_broadcast(cond);
}
#endif

#endif
