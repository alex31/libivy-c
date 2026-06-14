#include "ivy.h"

#include <pthread.h>
#include <stdio.h>

#define WORKER_COUNT 4
#define SEND_ITERATIONS 200

static void *send_worker(void *arg)
{
	long worker_id = (long)arg;
	int i;

	for (i = 0; i < SEND_ITERATIONS; i++) {
		int sent = IvySendMsg("phase3-worker-%ld-%d", worker_id, i);
		if (sent != 0) {
			fprintf(stderr, "IvySendMsg before stop returned %d\n", sent);
			return (void *)1;
		}
		if (IvyGetLastError() != IVY_OK) {
			fprintf(stderr, "IvySendMsg before stop last_error=%d\n", IvyGetLastError());
			return (void *)2;
		}
	}

	return NULL;
}

static void *stopped_worker(void *arg)
{
	(void)arg;

	if (IvySendMsg("phase3-after-stop") != IVY_ESTOPPED) {
		fprintf(stderr, "IvySendMsg after stop did not return IVY_ESTOPPED\n");
		return (void *)3;
	}
	if (IvyGetLastError() != IVY_ESTOPPED) {
		fprintf(stderr, "IvySendMsg after stop last_error=%d\n", IvyGetLastError());
		return (void *)4;
	}

	return NULL;
}

static int run_workers(void *(*entry)(void *))
{
	pthread_t workers[WORKER_COUNT];
	int i;

	for (i = 0; i < WORKER_COUNT; i++) {
		if (pthread_create(&workers[i], NULL, entry, (void *)(long)i) != 0) {
			fprintf(stderr, "pthread_create failed\n");
			return 10;
		}
	}

	for (i = 0; i < WORKER_COUNT; i++) {
		void *result = NULL;

		if (pthread_join(workers[i], &result) != 0) {
			fprintf(stderr, "pthread_join failed\n");
			return 11;
		}
		if (result != NULL)
			return 12;
	}

	return 0;
}

int main(void)
{
	int status;

	if (IvyInit("phase3", NULL, NULL, NULL, NULL, NULL) != IVY_OK) {
		fprintf(stderr, "IvyInit failed with %d\n", IvyGetLastError());
		return 1;
	}

	status = run_workers(send_worker);
	if (status != 0)
		return status;

	if (IvyStop() != IVY_OK) {
		fprintf(stderr, "IvyStop failed with %d\n", IvyGetLastError());
		return 2;
	}

	status = run_workers(stopped_worker);
	if (status != 0)
		return status;

	if (IvyTerminate() != IVY_OK) {
		fprintf(stderr, "IvyTerminate failed with %d\n", IvyGetLastError());
		return 3;
	}

	return 0;
}
