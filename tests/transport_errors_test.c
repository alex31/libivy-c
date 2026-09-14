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

static void on_transport(IvyClientPtr app, void *data, IvyStatus status, int system_error)
{
    Fixture *f = data;
    const char *name = IvyContextGetApplicationName(f->sender, app);
    if (!pthread_equal(pthread_self(), f->sender_thread)) atomic_fetch_or(&f->bad_callback, 1);
    if (!name || strcmp(name, "transport-first") != 0) atomic_fetch_or(&f->bad_callback, 2);
    if (atomic_load(&f->disconnected)) atomic_fetch_or(&f->bad_callback, 4);
    /* These reentrant calls prove that notification holds neither send nor context locks. */
    assert(IvyContextSetTransportErrorCallback(f->sender, NULL, NULL) == IVY_OK);
    assert(IvyContextSendMsg(f->sender, "NO_MATCH") == 0);
    atomic_store(&f->error_status, status);
    atomic_store(&f->system_error, system_error);
    atomic_fetch_add(&f->errors, 1);
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
    assert(IvyContextSetTransportErrorCallback(f->sender, on_transport, f) == IVY_OK);
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
    IvySendReport report;
    int port = argc > 1 ? atoi(argv[1]) : 29400;
    int i, status;
    fifo_test();
    fixture_start(&f, port);
    report = (IvySendReport){9, 9, 9, 9};
    assert(IvyContextSendMsgEx(NULL, &report, "TEST") == IVY_EINVAL);
    assert(!report.matched && !report.accepted && !report.failed && !report.system_error);
    assert(IvyContextSendMsgEx(f.sender, NULL, "TEST") == IVY_EINVAL);
    assert(IvyContextSetTransportErrorCallback(NULL, NULL, NULL) == IVY_EINVAL);
    assert(IvyContextSendDirectMsg(f.sender, NULL, 0, "TEST") == IVY_EINVAL);
    assert(IvyContextSendDirectMsg(f.first, f.first_peer, 0, "TEST") == IVY_EINVAL);
    assert(IvyContextSendMsgEx(f.sender, &report, "TEST %c", 0) == IVY_EINVAL);
    for (const char *invalid = "\n\002\003"; *invalid; ++invalid) {
        assert(IvyContextSendMsgEx(f.sender, &report, "TEST %c", *invalid) == IVY_EINVAL);
        assert(!report.matched && !report.accepted && !report.failed && !report.system_error);
        assert(IvyContextSendDirectMsg(f.sender, f.first_peer, 0, (char *)invalid) == IVY_EINVAL);
    }
    assert(IvyContextSendMsgEx(f.sender, &report, "NOT_MATCHING") == IVY_OK);
    assert(report.matched == 0 && report.accepted == 0 && report.failed == 0);
    fail_malloc = 1;
    assert(IvyContextSendMsgEx(f.sender, &report, "TEST allocation") == IVY_ENOMEM);
    assert(report.matched == 0 && report.failed == 0);
    atomic_store(&fault_mode, BROKEN);
    status = IvyContextSendMsgEx(f.sender, &report, "TEST failure");
    assert(status == IVY_EIO && IvyGetLastError() == IVY_EIO);
    assert(report.matched == 2 && report.accepted == 1 && report.failed == 1 && report.system_error == EPIPE);
    wait_count(&f.second_messages, 1);
    wait_count(&f.errors, 1);
    wait_count(&f.disconnected, 1);
    assert(atomic_load(&send_calls) == 1 && !atomic_load(&f.bad_callback));
    assert(atomic_load(&f.error_status) == IVY_EIO && atomic_load(&f.system_error) == EPIPE);
    fixture_stop(&f);

    fixture_start(&f, port + 1);
    atomic_store(&fault_mode, PARTIAL_NO_MEMORY);
    assert(IvyContextSendMsg(f.sender, "TEST partial") == IVY_ENOMEM);
    wait_count(&f.errors, 1);
    wait_count(&f.second_messages, 1);
    assert(atomic_load(&f.first_messages) == 0 && atomic_load(&send_calls) == 1);
    assert(atomic_load(&f.error_status) == IVY_ENOMEM && atomic_load(&f.system_error) == 0);
    fixture_stop(&f);

    fixture_start(&f, port + 2);
    atomic_store(&fault_mode, BLOCKED);
    for (i = 0; i < 100; ++i) {
        status = IvyContextSendMsgEx(f.sender, &report, "TEST %080d", i);
        if (status != IVY_OK) break;
        assert(report.matched == 2 && report.accepted == 2 && report.failed == 0);
    }
    assert(i > 0 && i < 100 && status == IVY_EFIFOFULL);
    assert(report.matched == 2 && report.accepted == 1 && report.failed == 1 && !report.system_error);
    assert(!atomic_load(&f.errors));
    atomic_store(&fault_mode, NORMAL);
    wait_count(&f.first_messages, i);
    wait_count(&f.second_messages, i + 1);
    assert(IvyContextSendMsg(f.sender, "TEST after-full") == 2);
    wait_count(&f.first_messages, i + 1);
    wait_count(&f.second_messages, i + 2);
    fixture_stop(&f);

    fixture_start(&f, port + 3);
    atomic_store(&fault_mode, BLOCKED);
    assert(IvyContextSendMsgEx(f.sender, &report, "TEST deferred") == IVY_OK);
    assert(report.accepted == 2 && !report.failed);
    atomic_store(&fault_mode, BROKEN);
    wait_count(&f.errors, 1);
    wait_count(&f.disconnected, 1);
    assert(!atomic_load(&f.bad_callback) && atomic_load(&f.error_status) == IVY_EIO);
    assert(atomic_load(&f.system_error) == EPIPE);
    fixture_stop(&f);
    fixture_start(&f, port + 4);
    atomic_store(&fault_mode, BROKEN);
    assert(IvyContextSendDirectMsg(f.sender, f.first_peer, 7, "direct failure") == IVY_EIO);
    wait_count(&f.errors, 1);
    wait_count(&f.disconnected, 1);
    assert(!atomic_load(&f.bad_callback) && atomic_load(&f.system_error) == EPIPE);
    fixture_stop(&f);

    fixture_start(&f, port + 5);
    assert(IvyContextSetTransportErrorCallback(f.sender, NULL, NULL) == IVY_OK);
    atomic_store(&fault_mode, BROKEN);
    assert(IvyContextSendDirectMsg(f.sender, f.first_peer, 7, "no handler") == IVY_EIO);
    wait_count(&f.disconnected, 1);
    assert(!atomic_load(&f.errors));
    fixture_stop(&f);
    puts("C transport, partial fan-out, FIFO and deferred-error tests passed");
    return 0;
}
