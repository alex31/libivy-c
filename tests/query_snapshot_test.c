#include "ivy.h"
#include "ivy_query_internal.h"

#include <assert.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static _Thread_local int allocation_budget = -1;
void *__real_calloc(size_t, size_t);
char *__real_strdup(const char *);

static int fail_allocation(void)
{
    if (allocation_budget < 0) return 0;
    if (allocation_budget == 0) return 1;
    --allocation_budget;
    return 0;
}

void *__wrap_calloc(size_t count, size_t size)
{
    return fail_allocation() ? NULL : __real_calloc(count, size);
}

char *__wrap_strdup(const char *text)
{
    return fail_allocation() ? NULL : __real_strdup(text);
}

static void pause_briefly(void)
{
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
}

static void *run_loop(void *data)
{
    IvyContextMainLoop(data);
    return NULL;
}

static IvyClientPtr wait_peer(IvyContext *ctx, char *name, int present)
{
    for (int i = 0; i < 500; ++i) {
        IvyClientPtr peer = IvyContextGetApplication(ctx, name);
        if ((peer != NULL) == present) return peer;
        pause_briefly();
    }
    assert(!"peer discovery/disconnection timed out");
    return NULL;
}

static int contains(const IvyStringSnapshot *snapshot, const char *text)
{
    for (size_t i = 0; i < snapshot->count; ++i)
        if (strcmp(snapshot->items[i], text) == 0) return 1;
    return 0;
}

static void on_message(IvyClientPtr app, void *data, int argc, char **argv)
{
    (void)app; (void)data; (void)argc; (void)argv;
}

struct Writer { IvyContext *ctx; MsgRcvPtr binding; };

static void *change_regexps(void *data)
{
    struct Writer *writer = data;
    for (int i = 0; i < 200; ++i) {
        assert(IvyContextChangeMsg(writer->ctx, writer->binding, "%s",
            i % 2 ? "^CHANGED|WITH,DELIMITERS$" : "^ONE,([A-Z]+)$"));
    }
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char peer_name[] = "snapshot,peer|one";
    IvyContext *ctx = IvyContextCreate("snapshot-reader", NULL, NULL, NULL, NULL, NULL);
    IvyContext *peer = IvyContextCreate(peer_name, NULL, NULL, NULL, NULL, NULL);
    IvyContext *other = IvyContextCreate("other", NULL, NULL, NULL, NULL, NULL);
    IvyStringSnapshot empty = {0}, info = {0}, names = {0}, regexps = {0}, endpoint = {0};
    unsigned short service_port = 99;
    pthread_t ctx_loop, peer_loop;
    assert(ctx && peer && other);
    assert(IvyContextCopyApplicationsInternal(ctx, &empty) == IVY_OK);
    assert(!empty.count && !empty.items);
    assert(IvyContextCopyApplicationsInternal(NULL, &empty) == IVY_EINVAL);
    assert(IvyContextCopyApplicationsInternal(ctx, NULL) == IVY_EINVAL);
    assert(IvyContextCopyApplicationInternal(ctx, NULL, &empty) == IVY_EINVAL);
    assert(IvyContextCopyApplicationRegexpsInternal(ctx, NULL, &empty) == IVY_EINVAL);
    assert(IvyContextCopyApplicationInfoInternal(ctx, NULL, &empty, &service_port) == IVY_EINVAL);
    assert(service_port == 0 && !empty.items && !empty.count);
    assert(IvyContextCopyApplicationInfoInternal(ctx, NULL, &empty, NULL) == IVY_EINVAL);
    MsgRcvPtr first = IvyContextBindMsg(peer, on_message, NULL, "%s", "^ONE,([A-Z]+)$");
    MsgRcvPtr second = IvyContextBindMsg(peer, on_message, NULL, "%s", "^TWO[|,](.*)$");
    assert(first && second);
    assert(IvyContextStart(ctx, argv[1]) == IVY_OK);
    assert(IvyContextStart(peer, argv[1]) == IVY_OK);
    assert(pthread_create(&ctx_loop, NULL, run_loop, ctx) == 0);
    assert(pthread_create(&peer_loop, NULL, run_loop, peer) == 0);
    IvyClientPtr app = wait_peer(ctx, peer_name, 1);
    assert(IvyContextCopyApplicationInternal(ctx, app, &info) == IVY_OK);
    assert(info.count == 2 && strcmp(info.items[0], peer_name) == 0 && info.items[1][0]);
    for (int i = 0; i < 500; ++i) {
        assert(IvyContextCopyApplicationInfoInternal(ctx, app, &endpoint, &service_port) == IVY_OK);
        if (service_port) break;
        IvyStringSnapshotFreeInternal(&endpoint);
        pause_briefly();
    }
    struct in_addr numeric_address;
    assert(endpoint.count == 2 && strcmp(endpoint.items[0], peer_name) == 0);
    assert(inet_pton(AF_INET, endpoint.items[1], &numeric_address) == 1 && service_port > 0);
    unsigned short other_port = 99;
    assert(IvyContextCopyApplicationInfoInternal(other, app, &empty, &other_port) == IVY_EINVAL);
    assert(other_port == 0 && !empty.items);
    assert(IvyContextCopyApplicationsInternal(ctx, &names) == IVY_OK);
    assert(names.count == 1 && contains(&names, peer_name));
    for (int i = 0; i < 500; ++i) {
        assert(IvyContextCopyApplicationRegexpsInternal(ctx, app, &regexps) == IVY_OK);
        if (regexps.count == 2) break;
        IvyStringSnapshotFreeInternal(&regexps);
        pause_briefly();
    }
    assert(regexps.count == 2 && contains(&regexps, "^ONE,([A-Z]+)$") && contains(&regexps, "^TWO[|,](.*)$"));
    assert(IvyContextCopyApplicationInternal(other, app, &empty) == IVY_EINVAL);
    assert(IvyContextCopyApplicationRegexpsInternal(other, app, &empty) == IVY_EINVAL);
    assert(IvyContextSendDieMsg(other, app) == IVY_EINVAL);
    assert(IvyContextSendError(other, app, 7, "%s", "wrong context") == IVY_EINVAL);
    assert(!empty.items && !empty.count);

    for (int budget = 0; budget < 3; ++budget) {
        allocation_budget = budget;
        int result = IvyContextCopyApplicationInternal(ctx, app, &empty);
        allocation_budget = -1;
        assert(result == IVY_ENOMEM && !empty.items && !empty.count);
        IvyStringSnapshotFreeInternal(&empty);
        allocation_budget = budget;
        other_port = 99;
        result = IvyContextCopyApplicationInfoInternal(ctx, app, &empty, &other_port);
        allocation_budget = -1;
        assert(result == IVY_ENOMEM && !empty.items && !empty.count && other_port == 0);
        allocation_budget = budget;
        result = IvyContextCopyApplicationRegexpsInternal(ctx, app, &empty);
        allocation_budget = -1;
        assert(result == IVY_ENOMEM && !empty.items && !empty.count);
    }
    for (int budget = 0; budget < 2; ++budget) {
        allocation_budget = budget;
        int result = IvyContextCopyApplicationsInternal(ctx, &empty);
        allocation_budget = -1;
        assert(result == IVY_ENOMEM && !empty.items && !empty.count);
    }

    struct Writer writer = {peer, first};
    pthread_t worker;
    assert(pthread_create(&worker, NULL, change_regexps, &writer) == 0);
    for (int i = 0; i < 500; ++i) {
        assert(IvyContextCopyApplicationRegexpsInternal(ctx, app, &empty) == IVY_OK);
        assert(empty.count == 2 && contains(&empty, "^TWO[|,](.*)$"));
        assert(contains(&empty, "^ONE,([A-Z]+)$") || contains(&empty, "^CHANGED|WITH,DELIMITERS$"));
        IvyStringSnapshotFreeInternal(&empty);
    }
    assert(pthread_join(worker, NULL) == 0);
    assert(IvyContextStop(peer) == IVY_OK);
    assert(pthread_join(peer_loop, NULL) == 0);
    assert(IvyContextDestroy(peer) == IVY_OK);
    (void)wait_peer(ctx, peer_name, 0);
    assert(IvyContextCopyApplicationInternal(ctx, app, &empty) == IVY_EINVAL);
    assert(IvyContextCopyApplicationInfoInternal(ctx, app, &empty, &other_port) == IVY_EINVAL);
    assert(IvyContextCopyApplicationRegexpsInternal(ctx, app, &empty) == IVY_EINVAL);
    assert(IvyContextCopyApplicationsInternal(ctx, &empty) == IVY_OK && !empty.count);
    assert(IvyContextStop(ctx) == IVY_OK);
    assert(pthread_join(ctx_loop, NULL) == 0);
    assert(IvyContextCopyApplicationsInternal(ctx, &empty) == IVY_ESTOPPED);
    assert(IvyContextCopyApplicationInternal(ctx, app, &empty) == IVY_ESTOPPED);
    assert(IvyContextCopyApplicationInfoInternal(ctx, app, &empty, &other_port) == IVY_ESTOPPED);
    assert(IvyContextCopyApplicationRegexpsInternal(ctx, app, &empty) == IVY_ESTOPPED);
    assert(IvyContextDestroy(ctx) == IVY_OK);
    assert(IvyContextDestroy(other) == IVY_OK);

    /* Copies remain usable after regexp changes, disconnection and destruction. */
    assert(info.count == 2 && strcmp(info.items[0], peer_name) == 0 && info.items[1][0]);
    assert(names.count == 1 && contains(&names, peer_name));
    assert(contains(&regexps, "^ONE,([A-Z]+)$") && contains(&regexps, "^TWO[|,](.*)$"));
    assert(endpoint.count == 2 && strcmp(endpoint.items[0], peer_name) == 0 && service_port > 0);
    assert(inet_pton(AF_INET, endpoint.items[1], &numeric_address) == 1);
    IvyStringSnapshotFreeInternal(&endpoint);
    IvyStringSnapshotFreeInternal(&info);
    IvyStringSnapshotFreeInternal(&names);
    IvyStringSnapshotFreeInternal(&regexps);
    IvyStringSnapshotFreeInternal(&empty);
    IvyStringSnapshotFreeInternal(&empty);
    IvyStringSnapshotFreeInternal(NULL);
    puts("Private query snapshots: ownership, allocation errors and concurrent updates passed");
    return 0;
}
