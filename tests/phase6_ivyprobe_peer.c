#include "ivy.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static void *run_loop(void *data)
{
	IvyContextMainLoop((IvyContext *)data);
	return NULL;
}

int main(int argc, char **argv)
{
	IvyContext *ctx;
	pthread_t loop_thread;
	const char *name;
	const char *bus;
	const char *payload;
	int sent = 0;
	int i;

	if (argc != 4) {
		fprintf(stderr, "usage: %s NAME BUS PAYLOAD\n", argv[0]);
		return 2;
	}

	name = argv[1];
	bus = argv[2];
	payload = argv[3];

	ctx = IvyContextCreate(name, NULL, NULL, NULL, NULL, NULL);
	if (!ctx) {
		fprintf(stderr, "IvyContextCreate failed with %d\n", IvyGetLastError());
		return 1;
	}
	if (IvyContextStart(ctx, bus) != IVY_OK) {
		fprintf(stderr, "IvyContextStart failed with %d\n", IvyGetLastError());
		IvyContextDestroy(ctx);
		return 1;
	}
	if (pthread_create(&loop_thread, NULL, run_loop, ctx) != 0) {
		perror("pthread_create");
		IvyContextStop(ctx);
		IvyContextDestroy(ctx);
		return 1;
	}

	for (i = 0; i < 50; i++) {
		sent = IvyContextSendMsg(ctx, "PROBE_TEST %s", payload);
		if (sent > 0)
			break;
		usleep(100000);
	}

	IvyContextStop(ctx);
	pthread_join(loop_thread, NULL);
	IvyContextDestroy(ctx);

	if (sent <= 0) {
		fprintf(stderr, "%s did not reach ivyprobe on %s\n", name, bus);
		return 1;
	}
	return 0;
}
