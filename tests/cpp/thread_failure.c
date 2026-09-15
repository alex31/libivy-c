/* LD_PRELOAD fault injection for the std::thread creation error boundary. */
#include <errno.h>
#include <pthread.h>
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*entry)(void *), void *data) {
    (void)thread; (void)attr; (void)entry; (void)data;
    return EAGAIN;
}
