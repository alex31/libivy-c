#include "ivy.h"
#include "ivychannel.h"
#include "ivyloop.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

extern int IvyTestingSetDefaultContextState(IvyContextState state);
extern IvyContextState IvyTestingGetDefaultContextState(void);

struct WritableTest {
	Channel channel;
	int fd;
	int peer_fd;
	int write_seen;
	int write_on_loop_thread;
};

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void *run_loop(void *data)
{
	(void)data;
	IvyMainLoop();
	return NULL;
}

static int wait_loop_active(void)
{
	long start = now_ms();

	while (!IvyChannelLoopIsActive()) {
		if (now_ms() - start > 1000)
			return 0;
		usleep(1000);
	}
	return 1;
}

static int start_running_loop(pthread_t *thread)
{
	if (IvyInit("phase4", NULL, NULL, NULL, NULL, NULL) != IVY_OK) {
		fprintf(stderr, "IvyInit failed with %d\n", IvyGetLastError());
		return 1;
	}
	IvyTestingSetDefaultContextState(IVY_CTX_RUNNING);
	if (pthread_create(thread, NULL, run_loop, NULL) != 0) {
		fprintf(stderr, "pthread_create failed\n");
		return 1;
	}
	if (!wait_loop_active()) {
		fprintf(stderr, "IvyMainLoop did not become active\n");
		return 1;
	}
	return 0;
}

static int finish_context(void)
{
	if (IvyTerminate() != IVY_OK) {
		fprintf(stderr, "IvyTerminate failed with %d\n", IvyGetLastError());
		return 1;
	}
	return 0;
}

static int test_worker_stop_wakes_select(void)
{
	pthread_t thread;
	long start;
	long elapsed;

	if (start_running_loop(&thread))
		return 1;

	usleep(100000);
	start = now_ms();
	if (IvyStop() != IVY_OK) {
		fprintf(stderr, "IvyStop failed with %d\n", IvyGetLastError());
		return 1;
	}
	elapsed = now_ms() - start;
	pthread_join(thread, NULL);

	if (elapsed > 500) {
		fprintf(stderr, "IvyStop took too long: %ld ms\n", elapsed);
		return 1;
	}
	if (IvyTestingGetDefaultContextState() != IVY_CTX_STOPPED) {
		fprintf(stderr, "context did not stop\n");
		return 1;
	}
	return finish_context();
}

static void stop_from_control(void *data)
{
	int *ran_on_loop_thread = (int *)data;
	*ran_on_loop_thread = IvyChannelIsLoopThread();
	IvyStop();
}

static int test_control_queue_runs_on_loop_thread(void)
{
	pthread_t thread;
	int ran_on_loop_thread = 0;

	if (start_running_loop(&thread))
		return 1;

	if (IvyChannelPostControl(stop_from_control, &ran_on_loop_thread) != 0) {
		fprintf(stderr, "IvyChannelPostControl failed\n");
		return 1;
	}
	pthread_join(thread, NULL);

	if (!ran_on_loop_thread) {
		fprintf(stderr, "control callback did not run on loop thread\n");
		return 1;
	}
	return finish_context();
}

static void close_channel_fd(void *data)
{
	struct WritableTest *test = (struct WritableTest *)data;
	if (test->fd >= 0) {
		close(test->fd);
		test->fd = -1;
	}
}

static void ignore_read(Channel channel, IVY_HANDLE fd, void *data)
{
	(void)channel;
	(void)fd;
	(void)data;
}

static void writable_cb(Channel channel, IVY_HANDLE fd, void *data)
{
	struct WritableTest *test = (struct WritableTest *)data;
	(void)fd;

	test->write_seen = 1;
	test->write_on_loop_thread = IvyChannelIsLoopThread();
	IvyChannelClearWritableEvent(channel);
	IvyStop();
}

static int test_writable_watch_is_routed_to_loop(void)
{
	pthread_t thread;
	int fds[2];
	struct WritableTest test;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		perror("socketpair");
		return 1;
	}

	test.fd = fds[0];
	test.peer_fd = fds[1];
	test.write_seen = 0;
	test.write_on_loop_thread = 0;

	if (IvyInit("phase4", NULL, NULL, NULL, NULL, NULL) != IVY_OK) {
		fprintf(stderr, "IvyInit failed with %d\n", IvyGetLastError());
		return 1;
	}
	test.channel = IvyChannelAdd(test.fd, &test, close_channel_fd, ignore_read, writable_cb);
	IvyTestingSetDefaultContextState(IVY_CTX_RUNNING);

	if (pthread_create(&thread, NULL, run_loop, NULL) != 0) {
		fprintf(stderr, "pthread_create failed\n");
		return 1;
	}
	if (!wait_loop_active()) {
		fprintf(stderr, "IvyMainLoop did not become active\n");
		return 1;
	}

	IvyChannelAddWritableEvent(test.channel);
	pthread_join(thread, NULL);

	if (!test.write_seen) {
		fprintf(stderr, "writable callback was not called\n");
		return 1;
	}
	if (!test.write_on_loop_thread) {
		fprintf(stderr, "writable callback did not run on loop thread\n");
		return 1;
	}

	IvyChannelRemove(test.channel);
	IvyIdle();
	close(test.peer_fd);
	return finish_context();
}

int main(void)
{
	if (test_worker_stop_wakes_select())
		return 1;
	if (test_control_queue_runs_on_loop_thread())
		return 1;
	if (test_writable_watch_is_routed_to_loop())
		return 1;

	return 0;
}
