#include "ivy.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
	IvyContext *ctx;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int got_1;
	int got_5;
} TimerPeerState;

static void *run_loop(void *data)
{
	IvyContextMainLoop((IvyContext *)data);
	return NULL;
}

static void timer_callback(IvyClientPtr app, void *user_data,
			   int argc, char *argv[])
{
	TimerPeerState *state = (TimerPeerState *)user_data;
	int done;

	(void)app;
	if (argc < 1)
		return;

	pthread_mutex_lock(&state->mutex);
	if (strcmp(argv[0], "1") == 0)
		state->got_1 = 1;
	if (strcmp(argv[0], "5") == 0)
		state->got_5 = 1;
	done = state->got_1 && state->got_5;
	if (done)
		pthread_cond_broadcast(&state->cond);
	pthread_mutex_unlock(&state->mutex);

	if (done)
		IvyContextStop(state->ctx);
}

int main(int argc, char **argv)
{
	TimerPeerState state;
	pthread_t loop_thread;
	struct timespec deadline;
	const char *name;
	const char *bus;
	int done = 0;
	int wait_status = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: %s NAME BUS\n", argv[0]);
		return 2;
	}
	name = argv[1];
	bus = argv[2];

	memset(&state, 0, sizeof(state));
	pthread_mutex_init(&state.mutex, NULL);
	pthread_cond_init(&state.cond, NULL);

	state.ctx = IvyContextCreate(name, NULL, NULL, NULL, NULL, NULL);
	if (!state.ctx) {
		fprintf(stderr, "IvyContextCreate failed with %d\n", IvyGetLastError());
		return 1;
	}
	if (!IvyContextBindMsg(state.ctx, timer_callback, &state,
			       "^TEST TIMER ([0-9]+)")) {
		fprintf(stderr, "IvyContextBindMsg failed with %d\n", IvyGetLastError());
		IvyContextDestroy(state.ctx);
		return 1;
	}
	if (IvyContextStart(state.ctx, bus) != IVY_OK) {
		fprintf(stderr, "IvyContextStart failed with %d\n", IvyGetLastError());
		IvyContextDestroy(state.ctx);
		return 1;
	}
	if (pthread_create(&loop_thread, NULL, run_loop, state.ctx) != 0) {
		perror("pthread_create");
		IvyContextStop(state.ctx);
		IvyContextDestroy(state.ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 12;
	pthread_mutex_lock(&state.mutex);
	while (!(state.got_1 && state.got_5) && wait_status != ETIMEDOUT)
		wait_status = pthread_cond_timedwait(&state.cond,
			&state.mutex, &deadline);
	done = state.got_1 && state.got_5;
	pthread_mutex_unlock(&state.mutex);

	IvyContextStop(state.ctx);
	pthread_join(loop_thread, NULL);
	IvyContextDestroy(state.ctx);
	pthread_cond_destroy(&state.cond);
	pthread_mutex_destroy(&state.mutex);

	if (!done) {
		fprintf(stderr, "%s on %s did not receive TEST TIMER 1 and 5\n",
			name, bus);
		return 1;
	}
	return 0;
}
