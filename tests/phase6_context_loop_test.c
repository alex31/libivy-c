#include "ivy.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int IvyTestingContextLoopIsActive(IvyContext *ctx);

static void *run_loop(void *data)
{
	IvyContextMainLoop((IvyContext *)data);
	return NULL;
}

static int wait_loop_active(IvyContext *ctx, const char *label)
{
	int i;

	for (i = 0; i < 100; i++) {
		if (IvyTestingContextLoopIsActive(ctx))
			return 0;
		usleep(10000);
	}

	fprintf(stderr, "%s loop did not become active\n", label);
	return 1;
}

static int expect_status(const char *label, int got, int expected)
{
	if (got == expected)
		return 0;

	fprintf(stderr, "%s returned %d, expected %d\n", label, got, expected);
	return 1;
}

int main(int argc, char **argv)
{
	IvyContext *ctx_a;
	IvyContext *ctx_b;
	pthread_t thread_a;
	pthread_t thread_b;
	int thread_a_started = 0;
	int thread_b_started = 0;
	const char *bus_a;
	const char *bus_b;
	int failed = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: %s BUS_A BUS_B\n", argv[0]);
		return 2;
	}

	bus_a = argv[1];
	bus_b = argv[2];

	ctx_a = IvyContextCreate("phase6-a", NULL, NULL, NULL, NULL, NULL);
	ctx_b = IvyContextCreate("phase6-b", NULL, NULL, NULL, NULL, NULL);
	if (!ctx_a || !ctx_b) {
		fprintf(stderr, "IvyContextCreate failed, last error %d\n", IvyGetLastError());
		return 1;
	}

	failed |= expect_status("start ctx_a", IvyContextStart(ctx_a, bus_a), IVY_OK);
	failed |= expect_status("start ctx_b", IvyContextStart(ctx_b, bus_b), IVY_OK);
	if (failed)
		goto cleanup;

	if (pthread_create(&thread_a, NULL, run_loop, ctx_a) != 0) {
		perror("pthread_create ctx_a");
		failed = 1;
		goto cleanup;
	}
	thread_a_started = 1;
	if (pthread_create(&thread_b, NULL, run_loop, ctx_b) != 0) {
		perror("pthread_create ctx_b");
		(void)IvyContextStop(ctx_a);
		(void)pthread_join(thread_a, NULL);
		thread_a_started = 0;
		failed = 1;
		goto cleanup;
	}
	thread_b_started = 1;

	failed |= wait_loop_active(ctx_a, "ctx_a");
	failed |= wait_loop_active(ctx_b, "ctx_b");

	if (!failed) {
		failed |= expect_status("stop ctx_a", IvyContextStop(ctx_a), IVY_OK);
		(void)pthread_join(thread_a, NULL);
		thread_a_started = 0;

		if (!IvyTestingContextLoopIsActive(ctx_b)) {
			fprintf(stderr, "ctx_b loop stopped when ctx_a stopped\n");
			failed = 1;
		}
	}

	if (thread_b_started) {
		failed |= expect_status("stop ctx_b", IvyContextStop(ctx_b), IVY_OK);
		(void)pthread_join(thread_b, NULL);
		thread_b_started = 0;
	}

cleanup:
	if (thread_a_started) {
		(void)IvyContextStop(ctx_a);
		(void)pthread_join(thread_a, NULL);
	}
	if (thread_b_started) {
		(void)IvyContextStop(ctx_b);
		(void)pthread_join(thread_b, NULL);
	}
	failed |= expect_status("destroy ctx_a", IvyContextDestroy(ctx_a), IVY_OK);
	failed |= expect_status("destroy ctx_b", IvyContextDestroy(ctx_b), IVY_OK);
	return failed ? 1 : 0;
}
