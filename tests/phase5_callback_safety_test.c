#include "ivy.h"
#include "ivyloop.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static MsgRcvPtr secondary_binding;
static int receiver_callback_count;
static int receiver_unbind_status = IVY_ESTATE;
static int receiver_stop_status = IVY_ESTATE;
static int sender_sent_count = -1;
static int sender_stopped;

static void timeout_handler(int sig)
{
	(void)sig;
	fprintf(stderr, "phase5 callback safety test timed out\n");
	_exit(124);
}

static void ignore_msg_cb(IvyClientPtr app, void *user_data, int argc, char **argv)
{
	(void)app;
	(void)user_data;
	(void)argc;
	(void)argv;
}

static void receiver_msg_cb(IvyClientPtr app, void *user_data, int argc, char **argv)
{
	(void)app;
	(void)user_data;
	(void)argc;
	(void)argv;

	receiver_callback_count++;
	receiver_unbind_status = IvyUnbindMsg(secondary_binding);
	secondary_binding = NULL;
	receiver_stop_status = IvyStop();
}

static int run_receiver(const char *self, const char *bus)
{
	pid_t child;
	int status;
	MsgRcvPtr primary_binding;

	child = fork();
	if (child < 0) {
		perror("fork");
		return 1;
	}
	if (child == 0) {
		execl(self, self, "sender", bus, (char *)NULL);
		perror("execl sender");
		_exit(127);
	}

	if (IvyInit("phase5_receiver", NULL, NULL, NULL, NULL, NULL) != IVY_OK) {
		fprintf(stderr, "receiver IvyInit failed with %d\n", IvyGetLastError());
		return 1;
	}
	primary_binding = IvyBindMsg(receiver_msg_cb, NULL, "^phase5 trigger (.*)$");
	secondary_binding = IvyBindMsg(ignore_msg_cb, NULL, "^phase5 secondary (.*)$");
	if (!primary_binding || !secondary_binding) {
		fprintf(stderr, "receiver IvyBindMsg failed with %d\n", IvyGetLastError());
		return 1;
	}
	if (IvyStart(bus) != IVY_OK) {
		fprintf(stderr, "receiver IvyStart failed with %d\n", IvyGetLastError());
		return 1;
	}

	IvyMainLoop();

	if (waitpid(child, &status, 0) < 0) {
		perror("waitpid");
		return 1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "sender failed, status=%d\n", status);
		return 1;
	}
	if (receiver_callback_count != 1) {
		fprintf(stderr, "receiver callback count=%d expected=1\n", receiver_callback_count);
		return 1;
	}
	if (receiver_unbind_status != IVY_OK) {
		fprintf(stderr, "IvyUnbindMsg from callback returned %d\n", receiver_unbind_status);
		return 1;
	}
	if (receiver_stop_status != IVY_OK) {
		fprintf(stderr, "IvyStop from callback returned %d\n", receiver_stop_status);
		return 1;
	}
	if (IvyTerminate() != IVY_OK) {
		fprintf(stderr, "receiver IvyTerminate failed with %d\n", IvyGetLastError());
		return 1;
	}
	return 0;
}

static void sender_app_cb(IvyClientPtr app, void *user_data, IvyApplicationEvent event)
{
	(void)app;
	(void)user_data;

	if (event != IvyApplicationConnected || sender_stopped)
		return;

	sender_sent_count = IvySendMsg("phase5 trigger payload");
	sender_stopped = 1;
	(void)IvyStop();
}

static int run_sender(const char *bus)
{
	usleep(300000);
	if (IvyInit("phase5_sender", NULL, sender_app_cb, NULL, NULL, NULL) != IVY_OK) {
		fprintf(stderr, "sender IvyInit failed with %d\n", IvyGetLastError());
		return 1;
	}
	if (IvyStart(bus) != IVY_OK) {
		fprintf(stderr, "sender IvyStart failed with %d\n", IvyGetLastError());
		return 1;
	}
	IvyMainLoop();
	if (sender_sent_count < 1) {
		fprintf(stderr, "sender sent_count=%d expected >=1\n", sender_sent_count);
		return 1;
	}
	if (IvyTerminate() != IVY_OK) {
		fprintf(stderr, "sender IvyTerminate failed with %d\n", IvyGetLastError());
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	signal(SIGALRM, timeout_handler);
	alarm(8);

	if (argc == 3 && strcmp(argv[1], "sender") == 0)
		return run_sender(argv[2]);

	if (argc != 2) {
		fprintf(stderr, "usage: %s BUS\n", argv[0]);
		return 1;
	}

	return run_receiver(argv[0], argv[1]);
}
