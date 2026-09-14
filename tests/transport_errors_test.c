#define _POSIX_C_SOURCE 200809L
#include "ivy.h"
#include "ivyfifo.h"
#include "ivysocket.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef TRANSPORT_GLIB
#include <glib.h>
#endif

enum { NORMAL, BLOCKED, BROKEN, PARTIAL_NO_MEMORY };
static atomic_int fault_fd = -1, fault_mode, arming, send_calls;
static _Thread_local int fail_malloc;
void *__real_malloc(size_t size);
ssize_t __real_send(int fd, const void *buffer, size_t length, int flags);

void *__wrap_malloc(size_t size)
{
    if (fail_malloc) { fail_malloc = 0; errno = ENOMEM; return NULL; }
    return __real_malloc(size);
}

static int contains(const char *buffer, size_t length, const char *word)
{
    size_t n = strlen(word), i;
    for (i = 0; i + n <= length; ++i)
        if (memcmp(buffer + i, word, n) == 0) return 1;
    return 0;
}

ssize_t __wrap_send(int fd, const void *buffer, size_t length, int flags)
{
    if (atomic_load(&arming) && contains(buffer, length, "ARM_TRANSPORT"))
        atomic_store(&fault_fd, fd);
    if (fd == atomic_load(&fault_fd)) {
        int mode = atomic_load(&fault_mode);
        if (mode != NORMAL) atomic_fetch_add(&send_calls, 1);
        if (mode == BLOCKED) { errno = EAGAIN; return -1; }
        if (mode == BROKEN) { errno = EPIPE; return -1; }
        if (mode == PARTIAL_NO_MEMORY && length > 1) {
            ssize_t result = __real_send(fd, buffer, 1, flags);
            if (result == 1) fail_malloc = 1;
            return result;
        }
    }
    return __real_send(fd, buffer, length, flags);
}

static void pause_briefly(void)
{
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}

static void wait_count(atomic_int *count, int expected)
{
    int i;
    for (i = 0; i < 5000 && atomic_load(count) < expected; ++i) pause_briefly();
    assert(atomic_load(count) >= expected);
}

typedef struct {
    IvyContext *sender, *first, *second;
    IvyClientPtr first_peer;
    pthread_t sender_thread, first_thread, second_thread;
    atomic_int first_messages, second_messages, errors, disconnected;
    atomic_int error_status, system_error, bad_callback;
} Fixture;

static void *loop(void *data) { IvyContextMainLoop(data); return NULL; }

static void message(IvyClientPtr app, void *data, int argc, char **argv)
{
    (void)app;
    assert(argc == 1 && argv && argv[0]);
    atomic_fetch_add((atomic_int *)data, 1);
}

static void on_application(IvyClientPtr app, void *data, IvyApplicationEvent event)
{
    Fixture *f = data;
    if (event == IvyApplicationDisconnected && app == f->first_peer)
        atomic_fetch_add(&f->disconnected, 1);
}



static IvyClientPtr wait_peer(IvyContext *ctx, const char *name)
{
    int i;
    IvyClientPtr app = NULL;
    for (i = 0; i < 5000 && !(app = IvyContextGetApplication(ctx, (char *)name)); ++i)
        pause_briefly();
    assert(app);
    return app;
}

static IvyContext *create_context(const char *name, IvyApplicationCallback callback, void *data)
{
#ifdef TRANSPORT_GLIB
    /* Each loop thread must drive its own GMainContext. */
    GMainContext *main_context = g_main_context_new();
    g_main_context_push_thread_default(main_context);
#endif
    IvyContext *ctx = IvyContextCreate(name, NULL, callback, data, NULL, NULL);
#ifdef TRANSPORT_GLIB
    g_main_context_pop_thread_default(main_context);
    g_main_context_unref(main_context);
#endif
    return ctx;
}

static void fixture_start(Fixture *f, int port)
{
    char bus[64];
    int i;
    memset(f, 0, sizeof(*f));
    atomic_store(&fault_fd, -1);
    atomic_store(&fault_mode, NORMAL);
    atomic_store(&send_calls, 0);
    snprintf(bus, sizeof(bus), "127.255.255.255:%d", port);
    f->sender = create_context("transport-sender", on_application, f);
    f->first = create_context("transport-first", NULL, NULL);
    f->second = create_context("transport-second", NULL, NULL);
    assert(f->sender && f->first && f->second);
    assert(IvyContextBindMsg(f->first, message, &f->first_messages, "^TEST (.*)$"));
    assert(IvyContextBindMsg(f->second, message, &f->second_messages, "^TEST (.*)$"));
    assert(IvyContextStart(f->first, bus) == IVY_OK);
    assert(IvyContextStart(f->second, bus) == IVY_OK);
    assert(pthread_create(&f->first_thread, NULL, loop, f->first) == 0);
    assert(pthread_create(&f->second_thread, NULL, loop, f->second) == 0);
    assert(IvyContextStart(f->sender, bus) == IVY_OK);
    assert(pthread_create(&f->sender_thread, NULL, loop, f->sender) == 0);
    f->first_peer = wait_peer(f->sender, "transport-first");
    (void)wait_peer(f->sender, "transport-second");
    for (i = 0; i < 5000; ++i) {
        char buffer[256];
        IvyClientPtr second = IvyContextGetApplication(f->sender, "transport-second");
        int a = IvyContextGetApplicationMessagesBuffer(f->sender, f->first_peer, buffer, sizeof(buffer), "\n");
        int first_ready = a > 1 && strstr(buffer, "^TEST");
        int b = IvyContextGetApplicationMessagesBuffer(f->sender, second, buffer, sizeof(buffer), "\n");
        if (first_ready && b > 1 && strstr(buffer, "^TEST")) break;
        pause_briefly();
    }
    assert(i < 5000);
    atomic_store(&arming, 1);
    assert(IvyContextSendDirectMsg(f->sender, f->first_peer, 99, "ARM_TRANSPORT") == IVY_OK);
    atomic_store(&arming, 0);
    assert(atomic_load(&fault_fd) >= 0);
}

static void fixture_stop(Fixture *f)
{
    atomic_store(&fault_mode, NORMAL);
    assert(IvyContextStop(f->sender) == IVY_OK);
    assert(IvyContextStop(f->first) == IVY_OK);
    assert(IvyContextStop(f->second) == IVY_OK);
    pthread_join(f->sender_thread, NULL);
    pthread_join(f->first_thread, NULL);
    pthread_join(f->second_thread, NULL);
    assert(IvyContextDestroy(f->sender) == IVY_OK);
    assert(IvyContextDestroy(f->first) == IVY_OK);
    assert(IvyContextDestroy(f->second) == IVY_OK);
}

static void fifo_test(void)
{
    IvyFifoBuffer *f = IvyFifoNew();
    char data[600], out[32];
    unsigned int remaining;
    int error;
    memset(data, 'x', sizeof(data));
    assert(f);
    assert(IvyFifoWriteChecked(f, "original", 8) == IVY_OK);
    fail_malloc = 1;
    assert(IvyFifoWriteChecked(f, data, 200) == IVY_ENOMEM);
    assert(IvyFifoLength(f) == 8);
    assert(IvyFifoWriteChecked(f, data, sizeof(data)) == IVY_EFIFOFULL);
    assert(IvyFifoLength(f) == 8);
    assert(IvyFifoFlush(f, -1, &remaining, &error) == IVY_EIO);
    assert(remaining == 8 && error == EBADF);
    assert(IvyFifoRead(f, out, sizeof(out)) == 8 && memcmp(out, "original", 8) == 0);
    /* Grow a wrapped ring while preserving the old tail and head. */
    assert(IvyFifoWriteChecked(f, data, 100) == IVY_OK);
    assert(IvyFifoRead(f, data, 90) == 90);
    assert(IvyFifoWriteChecked(f, data, 90) == IVY_OK);
    fail_malloc = 1;
    assert(IvyFifoWriteChecked(f, data, 200) == IVY_ENOMEM);
    assert(IvyFifoLength(f) == 100);
    assert(IvyFifoWriteChecked(f, data, 200) == IVY_OK);
    assert(IvyFifoRead(f, data, sizeof(data)) == 300);
    for (unsigned j = 0; j < 300; ++j) assert(data[j] == 'x');
    IvyFifoDelete(f);
}

int main(int argc, char **argv)
{
    Fixture f;
    int port = argc > 1 ? atoi(argv[1]) : 29400;
    int status = IVY_OK;
    fifo_test();
    fixture_start(&f, port);
    atomic_store(&fault_mode, BROKEN);
    assert(IvyContextSendDirectMsg(f.sender, f.first_peer, 7, "broken") == IVY_EIO);
    wait_count(&f.disconnected, 1);
    fixture_stop(&f);
    fixture_start(&f, port + 1);
    atomic_store(&fault_mode, PARTIAL_NO_MEMORY);
    assert(IvyContextSendDirectMsg(f.sender, f.first_peer, 7, "partial") == IVY_ENOMEM);
    wait_count(&f.disconnected, 1);
    fixture_stop(&f);
    fixture_start(&f, port + 2);
    atomic_store(&fault_mode, BLOCKED);
    for (int i = 0; i < 100 && status == IVY_OK; ++i)
        status = IvyContextSendDirectMsg(f.sender, f.first_peer, 7,
            "full FIFO: the complete frame must fit or be rejected without changing queued bytes");
    assert(status == IVY_EFIFOFULL);
    atomic_store(&fault_mode, NORMAL);
    for (int i = 0; i < 5000; ++i) {
        status = IvyContextSendDirectMsg(f.sender, f.first_peer, 7, "recovered");
        if (status == IVY_OK) break;
        assert(status == IVY_EFIFOFULL);
        pause_briefly();
    }
    assert(status == IVY_OK && !atomic_load(&f.disconnected));
    fixture_stop(&f);
    puts("Checked FIFO and partial-frame transport tests passed");
    return 0;
}
