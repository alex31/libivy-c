#include <stdio.h>
#include <stdlib.h>

#include "ivy.h"
#include "ivychannel.h"
#include "ivysocket.h"

static void *dummy_create(Client client)
{
	(void)client;
	return NULL;
}

static void dummy_delete(Client client, const void *data)
{
	(void)client;
	(void)data;
}

static void dummy_decongestion(Client client, const void *data)
{
	(void)client;
	(void)data;
}

static void dummy_interpret(Client client, const void *data, char *line)
{
	(void)client;
	(void)data;
	(void)line;
}

static int expect_status(const char *label, int got, int expected)
{
	if (got == expected)
		return 0;
	fprintf(stderr, "%s: expected %d, got %d\n", label, expected, got);
	return 1;
}

static int expect_state(const char *label, IvyContext *ctx, IvyContextState expected)
{
	IvyContextState got = IvyContextGetState(ctx);
	if (got == expected)
		return 0;
	fprintf(stderr, "%s: expected state %d, got %d\n", label, expected, got);
	return 1;
}

static int expect_null_server(const char *label, int fail_step)
{
	Server server;

	IvyTestingSocketServerFailStep(fail_step);
	server = SocketServerFor(NULL, 0, ANYPORT, dummy_create, dummy_delete,
				 dummy_decongestion, dummy_interpret);
	IvyTestingSocketServerFailStep(IVY_TEST_SOCKET_SERVER_FAIL_NONE);
	if (!server)
		return 0;

	fprintf(stderr, "%s: expected SocketServerFor failure\n", label);
	SocketServerClose(server);
	return 1;
}

static int expect_start_io_failure(const char *label,
				   int channel_fail_step,
				   int socket_fail_step)
{
	int failed = 0;
	IvyContext *ctx = IvyContextCreate("phase11", NULL, NULL, NULL, NULL, NULL);
	if (!ctx) {
		fprintf(stderr, "%s: IvyContextCreate failed\n", label);
		return 1;
	}

	IvyTestingChannelInitFailStep(channel_fail_step);
	IvyTestingSocketServerFailStep(socket_fail_step);
	failed |= expect_status(label, IvyContextStart(ctx, "127.255.255.255:29991"), IVY_EIO);
	failed |= expect_status("last error", IvyGetLastError(), IVY_EIO);
	failed |= expect_state("state after failed start", ctx, IVY_CTX_CREATED);
	IvyTestingChannelInitFailStep(IVY_TEST_CHANNEL_INIT_FAIL_NONE);
	IvyTestingSocketServerFailStep(IVY_TEST_SOCKET_SERVER_FAIL_NONE);

	if (IvyContextDestroy(ctx) != IVY_OK) {
		fprintf(stderr, "%s: IvyContextDestroy failed\n", label);
		failed = 1;
	}
	return failed;
}

int main(void)
{
	int failed = 0;

	failed |= expect_null_server("server socket failure",
				     IVY_TEST_SOCKET_SERVER_FAIL_SOCKET);
	failed |= expect_null_server("server reuseaddr failure",
				     IVY_TEST_SOCKET_SERVER_FAIL_REUSEADDR);
#ifdef SO_REUSEPORT
	failed |= expect_null_server("server reuseport failure",
				     IVY_TEST_SOCKET_SERVER_FAIL_REUSEPORT);
#endif
	failed |= expect_null_server("server bind failure",
				     IVY_TEST_SOCKET_SERVER_FAIL_BIND);
	failed |= expect_null_server("server getsockname failure",
				     IVY_TEST_SOCKET_SERVER_FAIL_GETSOCKNAME);
	failed |= expect_null_server("server listen failure",
				     IVY_TEST_SOCKET_SERVER_FAIL_LISTEN);

	failed |= expect_start_io_failure("context channel control failure",
					  IVY_TEST_CHANNEL_INIT_FAIL_CONTROL,
					  IVY_TEST_SOCKET_SERVER_FAIL_NONE);
	failed |= expect_start_io_failure("context channel wakeup failure",
					  IVY_TEST_CHANNEL_INIT_FAIL_WAKEUP,
					  IVY_TEST_SOCKET_SERVER_FAIL_NONE);
	failed |= expect_start_io_failure("context server socket failure",
					  IVY_TEST_CHANNEL_INIT_FAIL_NONE,
					  IVY_TEST_SOCKET_SERVER_FAIL_SOCKET);

	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
