#include "ivy.h"
#include "ivybind.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

extern int IvyTestingContextAcceptsFilter(IvyContext *, const char *);
extern int IvyLegacyDefaultContextIsInitialized(void);

/* Fail allocations made by the library during one filter operation only. */
static _Thread_local int allocation_budget = -1;
void *__real_malloc(size_t);
char *__real_strdup(const char *);

static int fail_allocation(void)
{
    if (allocation_budget < 0) return 0;
    if (allocation_budget == 0) return 1;
    --allocation_budget;
    return 0;
}

void *__wrap_malloc(size_t size)
{
    return fail_allocation() ? NULL : __real_malloc(size);
}

char *__wrap_strdup(const char *text)
{
    return fail_allocation() ? NULL : __real_strdup(text);
}

static void expect_status(int actual, IvyStatus expected)
{
    assert(actual == expected);
    assert(IvyGetLastError() == expected);
}

static int accepts(IvyContext *ctx, const char *regexp)
{
    return IvyTestingContextAcceptsFilter(ctx, regexp);
}

static void isolation_and_lifecycle(void)
{
    const char *a_words[] = {"TRACK"};
    const char *b_words[] = {"STATUS"};
    const char *duplicates[] = {"TRACK", "TRACK"};
    const char *bad_words[] = {"TRACK", NULL};
    char mutable_word[] = "COPIED";
    const char *mutable_words[] = {mutable_word};
    IvyContext *a = IvyContextCreate("filters-a", NULL, NULL, NULL, NULL, NULL);
    IvyContext *b = IvyContextCreate("filters-b", NULL, NULL, NULL, NULL, NULL);
    assert(a && b && !IvyLegacyDefaultContextIsInitialized());
    assert(accepts(a, "^TRACK") && accepts(b, "^TRACK"));
    expect_status(IvyContextSetFilter(a, 1, a_words), IVY_OK);
    expect_status(IvyContextSetFilter(b, 1, b_words), IVY_OK);
    assert(!IvyLegacyDefaultContextIsInitialized());
    assert(accepts(a, "^TRACK (.*)") && !accepts(a, "^STATUS (.*)"));
    assert(accepts(b, "^STATUS (.*)") && !accepts(b, "^TRACK (.*)"));
    assert(accepts(a, "^TRA.*") && !accepts(a, "^TRACKING"));
    assert(accepts(a, ".*") && accepts(a, "^(TRACK|STATUS)"));
    assert(accepts(a, "^") && accepts(a, ""));

    expect_status(IvyContextSetFilter(NULL, 1, a_words), IVY_EINVAL);
    expect_status(IvyContextAddFilter(NULL, "A"), IVY_EINVAL);
    expect_status(IvyContextRemoveFilter(NULL, "A"), IVY_EINVAL);
    expect_status(IvyContextSetFilter(a, -1, NULL), IVY_EINVAL);
    expect_status(IvyContextSetFilter(a, 1, NULL), IVY_EINVAL);
    expect_status(IvyContextSetFilter(a, 2, bad_words), IVY_EINVAL);
    expect_status(IvyContextAddFilter(a, NULL), IVY_EINVAL);
    expect_status(IvyContextAddFilter(a, ""), IVY_EINVAL);
    expect_status(IvyContextAddFilter(a, "bad word"), IVY_EINVAL);
    expect_status(IvyContextRemoveFilter(a, NULL), IVY_EINVAL);
    expect_status(IvyContextRemoveFilter(a, ""), IVY_EINVAL);
    assert(accepts(a, "^TRACK") && !accepts(a, "^STATUS"));

    expect_status(IvyContextSetFilter(a, 1, mutable_words), IVY_OK);
    mutable_word[0] = 'X';
    assert(accepts(a, "^COPIED") && !accepts(a, "^XOPIED"));
    assert(!accepts(a, "^TRACK")); // Set replaces; it no longer appends.
    expect_status(IvyContextSetFilter(a, 2, duplicates), IVY_OK);
    expect_status(IvyContextAddFilter(a, "TRACK"), IVY_OK);
    expect_status(IvyContextAddFilter(a, "OTHER"), IVY_OK);
    assert(accepts(a, "^OTHER") && !accepts(b, "^OTHER"));
    expect_status(IvyContextRemoveFilter(a, "OTHER"), IVY_OK);
    expect_status(IvyContextRemoveFilter(a, "OTHER"), IVY_OK);
    assert(!accepts(a, "^OTHER"));
    expect_status(IvyContextRemoveFilter(a, "TRACK"), IVY_OK);
    assert(accepts(a, "^ANYTHING")); // Removing the final class disables filtering.

    /* Legacy termination only destroys the default context's own filters. */
    expect_status(IvySetFilter(1, a_words), IVY_OK);
    assert(IvyBindingGetFilterCount() == 1 && IvyBindingFilter("^TRACK"));
    assert(!IvyBindingFilter("^STATUS"));
    expect_status(IvySetFilter(1, b_words), IVY_OK);
    assert(!IvyBindingFilter("^TRACK") && IvyBindingFilter("^STATUS"));
    IvyBindingSetFilter(2, duplicates);
    assert(IvyBindingGetFilterCount() == 1);
    IvyBindingAddFilter("EXTRA");
    assert(IvyBindingGetFilterCount() == 2);
    IvyBindingRemoveFilter("EXTRA");
    assert(IvyBindingGetFilterCount() == 1);
    IvyBindingTerminate();
    assert(IvyBindingGetFilterCount() == 0);
    expect_status(IvySetFilter(1, a_words), IVY_OK);
    expect_status(IvyTerminate(), IVY_OK);
    assert(!IvyLegacyDefaultContextIsInitialized());
    assert(accepts(b, "^STATUS") && !accepts(b, "^TRACK"));
    expect_status(IvyContextDestroy(a), IVY_OK);
    assert(accepts(b, "^STATUS") && !accepts(b, "^TRACK"));
    a = IvyContextCreate("fresh", NULL, NULL, NULL, NULL, NULL);
    assert(a && accepts(a, "^TRACK") && accepts(a, "^STATUS"));
    expect_status(IvyContextSetFilter(b, 0, NULL), IVY_OK);
    assert(accepts(b, "^TRACK"));
    expect_status(IvyContextStop(b), IVY_OK);
    expect_status(IvyContextSetFilter(b, 1, a_words), IVY_ESTOPPED);
    expect_status(IvyContextAddFilter(b, "A"), IVY_ESTOPPED);
    expect_status(IvyContextRemoveFilter(b, "A"), IVY_ESTOPPED);
    expect_status(IvyContextDestroy(a), IVY_OK);
    expect_status(IvyContextDestroy(b), IVY_OK);
}

static void allocation_failures(void)
{
    const char *old[] = {"KEEP"};
    const char *replacement[] = {"FIRST", "SECOND"};
    IvyContext *ctx = IvyContextCreate("allocation", NULL, NULL, NULL, NULL, NULL);
    assert(ctx);
    assert(IvyContextSetFilter(ctx, 1, old) == IVY_OK);
    for (int budget = 0; budget < 4; ++budget) {
        allocation_budget = budget;
        int result = IvyContextSetFilter(ctx, 2, replacement);
        allocation_budget = -1;
        expect_status(result, IVY_ENOMEM);
        assert(accepts(ctx, "^KEEP") && !accepts(ctx, "^FIRST") && !accepts(ctx, "^SECOND"));
    }
    for (int budget = 0; budget < 2; ++budget) {
        allocation_budget = budget;
        int result = IvyContextAddFilter(ctx, "NEW");
        allocation_budget = -1;
        expect_status(result, IVY_ENOMEM);
        assert(accepts(ctx, "^KEEP") && !accepts(ctx, "^NEW"));
    }
    allocation_budget = 0;
    int duplicate = IvyContextAddFilter(ctx, "KEEP");
    allocation_budget = -1;
    expect_status(duplicate, IVY_OK);
    expect_status(IvyContextSetFilter(ctx, 2, replacement), IVY_OK);
    assert(!accepts(ctx, "^KEEP") && accepts(ctx, "^FIRST") && accepts(ctx, "^SECOND"));
    expect_status(IvyContextDestroy(ctx), IVY_OK);
}

struct Worker { IvyContext *ctx; int writer; };

static void *concurrent_filters(void *data)
{
    struct Worker *worker = data;
    const char *left[] = {"LEFT", "COMMON"};
    const char *right[] = {"RIGHT", "COMMON"};
    for (int i = 0; i < 3000; ++i) {
        if (worker->writer) {
            assert(IvyContextSetFilter(worker->ctx, 2, i % 2 ? left : right) == IVY_OK);
            assert(IvyContextAddFilter(worker->ctx, "EXTRA") == IVY_OK);
            assert(IvyContextRemoveFilter(worker->ctx, "EXTRA") == IVY_OK);
        } else {
            assert(accepts(worker->ctx, "^COMMON"));
            assert(!accepts(worker->ctx, "^NEVER"));
        }
    }
    return NULL;
}

static void concurrent_updates(void)
{
    IvyContext *ctx = IvyContextCreate("concurrent", NULL, NULL, NULL, NULL, NULL);
    IvyContext *other = IvyContextCreate("independent", NULL, NULL, NULL, NULL, NULL);
    const char *common[] = {"COMMON"};
    const char *separate[] = {"OTHER"};
    struct Worker workers[4];
    pthread_t threads[4];
    assert(ctx && other);
    assert(IvyContextSetFilter(ctx, 1, common) == IVY_OK);
    assert(IvyContextSetFilter(other, 1, separate) == IVY_OK);
    for (int i = 0; i < 4; ++i) {
        workers[i] = (struct Worker){ctx, i < 2};
        assert(pthread_create(&threads[i], NULL, concurrent_filters, &workers[i]) == 0);
    }
    for (int i = 0; i < 3000; ++i) {
        assert(accepts(other, "^OTHER") && !accepts(other, "^COMMON"));
    }
    for (int i = 0; i < 4; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(IvyContextDestroy(ctx) == IVY_OK);
    assert(accepts(other, "^OTHER") && !accepts(other, "^COMMON"));
    assert(IvyContextDestroy(other) == IVY_OK);
}

int main(void)
{
    isolation_and_lifecycle();
    allocation_failures();
    concurrent_updates();
    puts("Per-context filters, lifecycle, allocation failure and concurrency tests passed");
    return 0;
}
