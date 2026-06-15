#include "ivy.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void *loop_thread(void *data)
{
	IvyContextMainLoop((IvyContext *)data);
	return NULL;
}

int main(int argc, char **argv)
{
	const char *bus = argc > 1 ? argv[1] : "127:23009";
	IvyContext *ctx;
	pthread_t thread;
	long start;
	long elapsed;
	int status;

	alarm(5);

	ctx = IvyContextCreate("phase9-wakeup", "phase9 ready", NULL, NULL, NULL, NULL);
	if (ctx == NULL) {
		fprintf(stderr, "IvyContextCreate failed: %d\n", IvyGetLastError());
		return 1;
	}

	status = IvyContextStart(ctx, bus);
	if (status != IVY_OK) {
		fprintf(stderr, "IvyContextStart failed: %d\n", status);
		IvyContextDestroy(ctx);
		return 1;
	}

	if (pthread_create(&thread, NULL, loop_thread, ctx) != 0) {
		fprintf(stderr, "pthread_create failed\n");
		IvyContextDestroy(ctx);
		return 1;
	}

	usleep(200000);
	start = now_ms();
	status = IvyContextStop(ctx);
	if (status != IVY_OK) {
		fprintf(stderr, "IvyContextStop failed: %d\n", status);
		return 1;
	}

	pthread_join(thread, NULL);
	elapsed = now_ms() - start;
	alarm(0);

	if (elapsed > 500) {
		fprintf(stderr, "IvyContextStop wakeup took too long: %ld ms\n", elapsed);
		IvyContextDestroy(ctx);
		return 1;
	}
	if (IvyContextGetState(ctx) != IVY_CTX_STOPPED) {
		fprintf(stderr, "context did not stop cleanly\n");
		IvyContextDestroy(ctx);
		return 1;
	}

	status = IvyContextDestroy(ctx);
	if (status != IVY_OK) {
		fprintf(stderr, "IvyContextDestroy failed: %d\n", status);
		return 1;
	}

	return 0;
}
