#include "ivybind.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct Bindings {
    IvyBinding small;
    IvyBinding large;
};

static void *worker(void *data)
{
    const struct Bindings *bindings = data;
    const char *argument;
    int length;

    /* Grow each thread's cache, then reuse it for both expression sizes. */
    for (int i = 0; i < 50; ++i) {
        assert(IvyBindingExec(bindings->small, "PING 42") == 2);
        IvyBindingMatch(bindings->small, "PING 42", 1, &length, &argument);
        assert(length == 2 && memcmp(argument, "42", 2) == 0);
        assert(IvyBindingExec(bindings->large, "VALUES 1 20 300") == 4);
        IvyBindingMatch(bindings->large, "VALUES 1 20 300", 3, &length, &argument);
        assert(length == 3 && memcmp(argument, "300", 3) == 0);
    }
    return NULL;
}

int main(void)
{
    const char *error;
    int offset;
    struct Bindings bindings = {
        IvyBindingCompile("^PING ([0-9]+)$", &offset, &error),
        IvyBindingCompile("^VALUES ([0-9]+) ([0-9]+) ([0-9]+)$", &offset, &error)
    };
    assert(bindings.small && bindings.large);
    for (int batch = 0; batch < 4; ++batch) {
        pthread_t threads[8];
        for (int i = 0; i < 8; ++i)
            assert(pthread_create(&threads[i], NULL, worker, &bindings) == 0);
        for (int i = 0; i < 8; ++i)
            assert(pthread_join(threads[i], NULL) == 0);
    }
    IvyBindingFree(bindings.small);
    IvyBindingFree(bindings.large);
    puts("Shared bindings, cache growth/reuse and 32 worker lifetimes passed");
    return 0;
}
