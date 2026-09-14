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
    IvyContext *ctx = IvyContextCreate(name, NULL, callback, data, NULL, NULL);
    return ctx;
}

static void fixture_start(Fixture *f, int port)
{
    char bus[64];
    int i;
    memset(f, 0, sizeof(*f));
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
}

static void fixture_stop(Fixture *f)
{
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



int main(int argc, char **argv)
{
    Fixture f;
    int port = argc > 1 ? atoi(argv[1]) : 29400;
    fixture_start(&f, port);
    assert(IvyContextSendMsg(f.sender, "TEST teardown") == 2);
    wait_count(&f.first_messages, 1);
    wait_count(&f.second_messages, 1);
    fixture_stop(&f);
    puts("Context teardown with remote regexp caches passed");
    return 0;
}
