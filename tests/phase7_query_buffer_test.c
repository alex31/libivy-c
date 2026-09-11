#include "ivy.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WAIT_STEPS 100
#define WAIT_USEC 100000

typedef struct {
	const char *bus;
	const char *rx_name;
	const char *tx_name;
	const char *regexp;
	IvyContext *rx;
	IvyContext *tx;
	IvyClientPtr app;
	pthread_t rx_thread;
	pthread_t tx_thread;
	int rx_thread_started;
	int tx_thread_started;
} BusFixture;

typedef struct {
	IvyContext *ctx;
	IvyClientPtr app;
	const char *app_name;
	const char *regexp;
	int failed;
} QueryWorker;

static void *run_loop(void *data)
{
	IvyContextMainLoop((IvyContext *)data);
	return NULL;
}

static int expect_status(const char *label, int got, int expected)
{
	if (got == expected)
		return 0;
	fprintf(stderr, "%s returned %d, expected %d\n", label, got, expected);
	return 1;
}

static int expect_last_error(const char *label, IvyStatus expected)
{
	IvyStatus got = IvyGetLastError();

	if (got == expected)
		return 0;
	fprintf(stderr, "%s last_error=%d, expected %d\n", label, got, expected);
	return 1;
}

static int wait_application(IvyContext *ctx, const char *name)
{
	int i;

	for (i = 0; i < WAIT_STEPS; i++) {
		if (IvyContextGetApplication(ctx, (char *)name))
			return 0;
		usleep(WAIT_USEC);
	}
	fprintf(stderr, "application %s not visible\n", name);
	return 1;
}

static void noop_message_callback(IvyClientPtr app, void *user_data,
				  int argc, char *argv[])
{
	(void)app;
	(void)user_data;
	(void)argc;
	(void)argv;
}

static int expect_buffer_contains(const char *label, const char *buffer,
				  const char *expected)
{
	if (strstr(buffer, expected))
		return 0;
	fprintf(stderr, "%s buffer '%s' does not contain '%s'\n",
		label, buffer, expected);
	return 1;
}

static int expect_buffer_not_contains(const char *label, const char *buffer,
				      const char *unexpected)
{
	if (!strstr(buffer, unexpected))
		return 0;
	fprintf(stderr, "%s buffer '%s' unexpectedly contains '%s'\n",
		label, buffer, unexpected);
	return 1;
}

static int start_fixture(BusFixture *fixture)
{
	int failed = 0;

	fixture->rx = IvyContextCreate(fixture->rx_name, NULL, NULL,
		NULL, NULL, NULL);
	fixture->tx = IvyContextCreate(fixture->tx_name, NULL, NULL,
		NULL, NULL, NULL);
	if (!fixture->rx || !fixture->tx) {
		fprintf(stderr, "IvyContextCreate failed with %d\n",
			IvyGetLastError());
		return 1;
	}

	if (!IvyContextBindMsg(fixture->rx, noop_message_callback, NULL,
			       "%s", fixture->regexp)) {
		fprintf(stderr, "IvyContextBindMsg failed with %d\n",
			IvyGetLastError());
		return 1;
	}

	failed |= expect_status("start rx",
		IvyContextStart(fixture->rx, fixture->bus), IVY_OK);
	failed |= expect_status("start tx",
		IvyContextStart(fixture->tx, fixture->bus), IVY_OK);
	if (failed)
		return failed;

	if (pthread_create(&fixture->rx_thread, NULL, run_loop,
			   fixture->rx) != 0) {
		perror("pthread_create rx");
		return 1;
	}
	fixture->rx_thread_started = 1;
	if (pthread_create(&fixture->tx_thread, NULL, run_loop,
			   fixture->tx) != 0) {
		perror("pthread_create tx");
		return 1;
	}
	fixture->tx_thread_started = 1;

	failed |= wait_application(fixture->tx, fixture->rx_name);
	if (failed)
		return failed;

	fixture->app = IvyContextGetApplication(fixture->tx,
		(char *)fixture->rx_name);
	if (!fixture->app) {
		fprintf(stderr, "IvyContextGetApplication failed with %d\n",
			IvyGetLastError());
		return 1;
	}
	return 0;
}

static int stop_fixture(BusFixture *fixture)
{
	int failed = 0;

	if (fixture->tx) {
		(void)IvyContextStop(fixture->tx);
		if (fixture->tx_thread_started) {
			pthread_join(fixture->tx_thread, NULL);
			fixture->tx_thread_started = 0;
		}
		failed |= expect_status("destroy tx",
			IvyContextDestroy(fixture->tx), IVY_OK);
		fixture->tx = NULL;
	}
	if (fixture->rx) {
		(void)IvyContextStop(fixture->rx);
		if (fixture->rx_thread_started) {
			pthread_join(fixture->rx_thread, NULL);
			fixture->rx_thread_started = 0;
		}
		failed |= expect_status("destroy rx",
			IvyContextDestroy(fixture->rx), IVY_OK);
		fixture->rx = NULL;
	}
	return failed;
}

static int check_query_contract(BusFixture *fixture,
				const char *forbidden_app,
				const char *forbidden_regexp)
{
	char apps[128];
	char messages[256];
	char small[4];
	char *exact_apps = NULL;
	char *exact_messages = NULL;
	int app_needed = 0;
	int msg_needed = 0;
	const char *name;
	const char *host;
	int needed;
	int failed = 0;

	name = IvyContextGetApplicationName(fixture->tx, fixture->app);
	if (!name || strcmp(name, fixture->rx_name) != 0) {
		fprintf(stderr, "application name '%s', expected '%s'\n",
			name ? name : "(null)", fixture->rx_name);
		failed = 1;
	}
	failed |= expect_last_error("application name", IVY_OK);

	host = IvyContextGetApplicationHost(fixture->tx, fixture->app);
	if (!host || !host[0]) {
		fprintf(stderr, "application host is empty\n");
		failed = 1;
	}
	failed |= expect_last_error("application host", IVY_OK);

	needed = IvyContextGetApplicationListBuffer(fixture->tx, NULL, 0, ",");
	if (needed <= 0)
		failed = 1;
	app_needed = needed;
	failed |= expect_last_error("application list sizing", IVY_OK);

	needed = IvyContextGetApplicationListBuffer(fixture->tx,
		small, sizeof(small), ",");
	if (needed <= (int)sizeof(small))
		failed = 1;
	failed |= expect_last_error("application list small buffer", IVY_ENOMEM);

	needed = IvyContextGetApplicationListBuffer(fixture->tx,
		apps, sizeof(apps), ",");
	if (needed <= 0)
		failed = 1;
	failed |= expect_last_error("application list full buffer", IVY_OK);
	failed |= expect_buffer_contains("application list",
		apps, fixture->rx_name);
	failed |= expect_buffer_not_contains("application list",
		apps, forbidden_app);

	if (app_needed > 0) {
		exact_apps = malloc((size_t)app_needed);
		if (!exact_apps) {
			perror("malloc exact_apps");
			failed = 1;
		} else {
			needed = IvyContextGetApplicationListBuffer(fixture->tx,
				exact_apps, (size_t)app_needed, ",");
			if (needed != app_needed) {
				fprintf(stderr, "application list exact buffer returned %d, expected %d\n",
					needed, app_needed);
				failed = 1;
			}
			failed |= expect_last_error("application list exact buffer", IVY_OK);
			failed |= expect_buffer_contains("application list exact",
				exact_apps, fixture->rx_name);
			failed |= expect_buffer_not_contains("application list exact",
				exact_apps, forbidden_app);
		}
	}

	needed = IvyContextGetApplicationMessagesBuffer(fixture->tx,
		fixture->app, NULL, 0, "|");
	if (needed <= 0)
		failed = 1;
	msg_needed = needed;
	failed |= expect_last_error("messages sizing", IVY_OK);

	needed = IvyContextGetApplicationMessagesBuffer(fixture->tx,
		fixture->app, small, sizeof(small), "|");
	if (needed <= (int)sizeof(small))
		failed = 1;
	failed |= expect_last_error("messages small buffer", IVY_ENOMEM);

	needed = IvyContextGetApplicationMessagesBuffer(fixture->tx,
		fixture->app, messages, sizeof(messages), "|");
	if (needed <= 0)
		failed = 1;
	failed |= expect_last_error("messages full buffer", IVY_OK);
	failed |= expect_buffer_contains("messages",
		messages, fixture->regexp);
	failed |= expect_buffer_not_contains("messages",
		messages, forbidden_regexp);

	if (msg_needed > 0) {
		exact_messages = malloc((size_t)msg_needed);
		if (!exact_messages) {
			perror("malloc exact_messages");
			failed = 1;
		} else {
			needed = IvyContextGetApplicationMessagesBuffer(fixture->tx,
				fixture->app, exact_messages, (size_t)msg_needed, "|");
			if (needed != msg_needed) {
				fprintf(stderr, "messages exact buffer returned %d, expected %d\n",
					needed, msg_needed);
				failed = 1;
			}
			failed |= expect_last_error("messages exact buffer", IVY_OK);
			failed |= expect_buffer_contains("messages exact",
				exact_messages, fixture->regexp);
			failed |= expect_buffer_not_contains("messages exact",
				exact_messages, forbidden_regexp);
		}
	}

	free(exact_apps);
	free(exact_messages);
	return failed;
}

static void *query_worker_main(void *data)
{
	QueryWorker *worker = (QueryWorker *)data;
	int i;

	for (i = 0; i < 1000; i++) {
		char apps[128];
		char messages[256];
		int len;

		len = IvyContextGetApplicationListBuffer(worker->ctx,
			apps, sizeof(apps), ",");
		if (len < 0 || IvyGetLastError() != IVY_OK ||
		    !strstr(apps, worker->app_name)) {
			worker->failed = 1;
			return NULL;
		}

		len = IvyContextGetApplicationMessagesBuffer(worker->ctx,
			worker->app, messages, sizeof(messages), "|");
		if (len < 0 || IvyGetLastError() != IVY_OK ||
		    !strstr(messages, worker->regexp)) {
			worker->failed = 1;
			return NULL;
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	BusFixture bus_a;
	BusFixture bus_b;
	pthread_t worker_a;
	pthread_t worker_b;
	int worker_a_started = 0;
	int worker_b_started = 0;
	QueryWorker query_a;
	QueryWorker query_b;
	int failed = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: %s BUS_A BUS_B\n", argv[0]);
		return 2;
	}

	memset(&bus_a, 0, sizeof(bus_a));
	memset(&bus_b, 0, sizeof(bus_b));
	bus_a.bus = argv[1];
	bus_a.rx_name = "phase7-rx-a";
	bus_a.tx_name = "phase7-tx-a";
	bus_a.regexp = "^PHASE7_A (.*)";
	bus_b.bus = argv[2];
	bus_b.rx_name = "phase7-rx-b";
	bus_b.tx_name = "phase7-tx-b";
	bus_b.regexp = "^PHASE7_B (.*)";

	if (start_fixture(&bus_a) || start_fixture(&bus_b)) {
		failed = 1;
		goto cleanup;
	}

	if (IvyContextGetApplicationName(bus_a.tx, bus_b.app)) {
		fprintf(stderr, "foreign application handle accepted by bus_a\n");
		failed = 1;
	}
	failed |= expect_last_error("foreign application name", IVY_EINVAL);
	if (IvyContextGetApplicationHost(bus_b.tx, bus_a.app)) {
		fprintf(stderr, "foreign application handle accepted by bus_b\n");
		failed = 1;
	}
	failed |= expect_last_error("foreign application host", IVY_EINVAL);

	failed |= check_query_contract(&bus_a, bus_b.rx_name, bus_b.regexp);
	failed |= check_query_contract(&bus_b, bus_a.rx_name, bus_a.regexp);
	if (failed)
		goto cleanup;

	query_a.ctx = bus_a.tx;
	query_a.app = bus_a.app;
	query_a.app_name = bus_a.rx_name;
	query_a.regexp = bus_a.regexp;
	query_a.failed = 0;
	query_b.ctx = bus_b.tx;
	query_b.app = bus_b.app;
	query_b.app_name = bus_b.rx_name;
	query_b.regexp = bus_b.regexp;
	query_b.failed = 0;

	if (pthread_create(&worker_a, NULL, query_worker_main, &query_a) != 0) {
		perror("pthread_create worker_a");
		failed = 1;
		goto cleanup;
	}
	worker_a_started = 1;
	if (pthread_create(&worker_b, NULL, query_worker_main, &query_b) != 0) {
		perror("pthread_create worker_b");
		failed = 1;
		goto cleanup;
	}
	worker_b_started = 1;

	pthread_join(worker_a, NULL);
	worker_a_started = 0;
	pthread_join(worker_b, NULL);
	worker_b_started = 0;
	if (query_a.failed || query_b.failed) {
		fprintf(stderr, "concurrent multibus buffer queries failed\n");
		failed = 1;
	}

	failed |= expect_status("stop bus_a tx",
		IvyContextStop(bus_a.tx), IVY_OK);
	if (bus_a.tx_thread_started) {
		pthread_join(bus_a.tx_thread, NULL);
		bus_a.tx_thread_started = 0;
	}
	{
		char apps[128];
		failed |= expect_status("query after stop",
			IvyContextGetApplicationListBuffer(bus_a.tx,
				apps, sizeof(apps), ","),
			IVY_ESTOPPED);
		failed |= expect_last_error("query after stop", IVY_ESTOPPED);
	}
	failed |= check_query_contract(&bus_b, bus_a.rx_name, bus_a.regexp);

cleanup:
	if (worker_a_started)
		pthread_join(worker_a, NULL);
	if (worker_b_started)
		pthread_join(worker_b, NULL);
	failed |= stop_fixture(&bus_b);
	failed |= stop_fixture(&bus_a);
	return failed ? 1 : 0;
}
