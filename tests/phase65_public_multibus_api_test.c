#include "ivy.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WAIT_STEPS 100
#define WAIT_USEC 100000
#define MAX_PAYLOADS 8

typedef struct {
	const char *payload;
	int count;
} PayloadCount;

typedef struct {
	const char *label;
	const char *tag;
	const char *direct_msg;
	int direct_id;
	PayloadCount payloads[MAX_PAYLOADS];
	int payload_count;
	int unexpected_tag_count;
	int malformed_count;
	int untracked_payload_count;
	int direct_count;
	pthread_mutex_t mutex;
} ReceiverState;

typedef struct {
	const char *label;
	const char *peer_name;
	int add_count;
	int change_count;
	int remove_count;
	pthread_mutex_t mutex;
} SenderState;

typedef struct {
	IvyContext *ctx;
	const char *name;
	pthread_t thread;
	int thread_started;
} ContextRun;

typedef struct {
	const char *rx_a_name;
	const char *rx_b_name;
	int pong_a_count;
	int pong_b_count;
	int unexpected_count;
	pthread_mutex_t mutex;
} PongState;

static PongState pong_state;

static void receiver_init(ReceiverState *state, const char *label,
			  const char *tag, const char *direct_msg, int direct_id)
{
	memset(state, 0, sizeof(*state));
	state->label = label;
	state->tag = tag;
	state->direct_msg = direct_msg;
	state->direct_id = direct_id;
	pthread_mutex_init(&state->mutex, NULL);
}

static void receiver_track(ReceiverState *state, const char *payload)
{
	if (state->payload_count >= MAX_PAYLOADS) {
		fprintf(stderr, "%s payload table is full\n", state->label);
		exit(2);
	}
	state->payloads[state->payload_count].payload = payload;
	state->payload_count++;
}

static void receiver_destroy(ReceiverState *state)
{
	pthread_mutex_destroy(&state->mutex);
}

static void sender_init(SenderState *state, const char *label, const char *peer_name)
{
	memset(state, 0, sizeof(*state));
	state->label = label;
	state->peer_name = peer_name;
	pthread_mutex_init(&state->mutex, NULL);
}

static void sender_destroy(SenderState *state)
{
	pthread_mutex_destroy(&state->mutex);
}

static void pong_state_init(const char *rx_a_name, const char *rx_b_name)
{
	memset(&pong_state, 0, sizeof(pong_state));
	pong_state.rx_a_name = rx_a_name;
	pong_state.rx_b_name = rx_b_name;
	pthread_mutex_init(&pong_state.mutex, NULL);
}

static void pong_state_destroy(void)
{
	pthread_mutex_destroy(&pong_state.mutex);
}

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

static int expect_positive_send(const char *label, int got)
{
	if (got > 0)
		return 0;
	fprintf(stderr, "%s sent to %d peers, expected at least one\n", label, got);
	return 1;
}

static int expect_zero_send(const char *label, int got)
{
	if (got == 0)
		return 0;
	fprintf(stderr, "%s sent to %d peers, expected zero\n", label, got);
	return 1;
}

static int start_context(ContextRun *run, const char *bus)
{
	if (IvyContextStart(run->ctx, bus) != IVY_OK) {
		fprintf(stderr, "IvyContextStart(%s) failed with %d\n",
			run->name, IvyGetLastError());
		return 1;
	}
	if (pthread_create(&run->thread, NULL, run_loop, run->ctx) != 0) {
		perror("pthread_create");
		(void)IvyContextStop(run->ctx);
		return 1;
	}
	run->thread_started = 1;
	return 0;
}

static int stop_context(ContextRun *run)
{
	int failed = 0;

	if (!run->ctx)
		return 0;
	failed |= expect_status(run->name, IvyContextStop(run->ctx), IVY_OK);
	if (run->thread_started) {
		pthread_join(run->thread, NULL);
		run->thread_started = 0;
	}
	return failed;
}

static int destroy_context(ContextRun *run)
{
	int failed = 0;

	if (!run->ctx)
		return 0;
	failed |= expect_status(run->name, IvyContextDestroy(run->ctx), IVY_OK);
	run->ctx = NULL;
	return failed;
}

static int receiver_payload_count(ReceiverState *state, const char *payload)
{
	int i;
	int count = 0;

	pthread_mutex_lock(&state->mutex);
	for (i = 0; i < state->payload_count; i++) {
		if (strcmp(state->payloads[i].payload, payload) == 0) {
			count = state->payloads[i].count;
			break;
		}
	}
	pthread_mutex_unlock(&state->mutex);
	return count;
}

static int receiver_direct_count(ReceiverState *state)
{
	int count;

	pthread_mutex_lock(&state->mutex);
	count = state->direct_count;
	pthread_mutex_unlock(&state->mutex);
	return count;
}

static int receiver_unexpected_count(ReceiverState *state)
{
	int count;

	pthread_mutex_lock(&state->mutex);
	count = state->unexpected_tag_count + state->malformed_count +
		state->untracked_payload_count;
	pthread_mutex_unlock(&state->mutex);
	return count;
}

static int wait_payload(ReceiverState *state, const char *payload,
			int min_count, const char *label)
{
	int i;

	for (i = 0; i < WAIT_STEPS; i++) {
		if (receiver_payload_count(state, payload) >= min_count)
			return 0;
		usleep(WAIT_USEC);
	}
	fprintf(stderr, "%s did not receive payload '%s' %d time(s), got %d\n",
		label, payload, min_count, receiver_payload_count(state, payload));
	return 1;
}

static int wait_direct(ReceiverState *state, int min_count, const char *label)
{
	int i;

	for (i = 0; i < WAIT_STEPS; i++) {
		if (receiver_direct_count(state) >= min_count)
			return 0;
		usleep(WAIT_USEC);
	}
	fprintf(stderr, "%s did not receive direct message %d time(s), got %d\n",
		label, min_count, receiver_direct_count(state));
	return 1;
}

static int expect_no_payload_after_wait(ReceiverState *state,
					const char *payload, const char *label)
{
	usleep(5 * WAIT_USEC);
	if (receiver_payload_count(state, payload) == 0)
		return 0;
	fprintf(stderr, "%s unexpectedly received payload '%s' %d time(s)\n",
		label, payload, receiver_payload_count(state, payload));
	return 1;
}

static int expect_no_unexpected(ReceiverState *state)
{
	int count = receiver_unexpected_count(state);

	if (count == 0)
		return 0;
	fprintf(stderr, "%s received %d unexpected/malformed message(s)\n",
		state->label, count);
	return 1;
}

static int sender_event_count(SenderState *state, IvyBindEvent event)
{
	int count;

	pthread_mutex_lock(&state->mutex);
	switch (event) {
	case IvyAddBind:
		count = state->add_count;
		break;
	case IvyChangeBind:
		count = state->change_count;
		break;
	case IvyRemoveBind:
		count = state->remove_count;
		break;
	default:
		count = 0;
		break;
	}
	pthread_mutex_unlock(&state->mutex);
	return count;
}

static int wait_sender_event(SenderState *state, IvyBindEvent event,
			     int min_count, const char *label)
{
	int i;

	for (i = 0; i < WAIT_STEPS; i++) {
		if (sender_event_count(state, event) >= min_count)
			return 0;
		usleep(WAIT_USEC);
	}
	fprintf(stderr, "%s did not observe bind event %d %d time(s), got %d\n",
		label, event, min_count, sender_event_count(state, event));
	return 1;
}

static int wait_pong_count(const char *name, int min_count, const char *label)
{
	int i;
	int count;

	for (i = 0; i < WAIT_STEPS; i++) {
		pthread_mutex_lock(&pong_state.mutex);
		if (strcmp(name, pong_state.rx_a_name) == 0)
			count = pong_state.pong_a_count;
		else
			count = pong_state.pong_b_count;
		pthread_mutex_unlock(&pong_state.mutex);
		if (count >= min_count)
			return 0;
		usleep(WAIT_USEC);
	}

	pthread_mutex_lock(&pong_state.mutex);
	if (strcmp(name, pong_state.rx_a_name) == 0)
		count = pong_state.pong_a_count;
	else
		count = pong_state.pong_b_count;
	pthread_mutex_unlock(&pong_state.mutex);
	fprintf(stderr, "%s did not receive pong from %s %d time(s), got %d\n",
		label, name, min_count, count);
	return 1;
}

static int wait_application(IvyContext *ctx, const char *name, const char *label)
{
	int i;

	for (i = 0; i < WAIT_STEPS; i++) {
		if (IvyContextGetApplication(ctx, (char *)name))
			return 0;
		usleep(WAIT_USEC);
	}
	fprintf(stderr, "%s did not see application %s\n", label, name);
	return 1;
}

static int expect_no_application(IvyContext *ctx, const char *name, const char *label)
{
	if (!IvyContextGetApplication(ctx, (char *)name))
		return 0;
	fprintf(stderr, "%s unexpectedly sees application %s\n", label, name);
	return 1;
}

static int expect_application_list(IvyContext *ctx, const char *present,
				   const char *absent, const char *label)
{
	char *list = IvyContextGetApplicationList(ctx, ",");

	if (!list) {
		fprintf(stderr, "%s application list is NULL\n", label);
		return 1;
	}
	if (!strstr(list, present)) {
		fprintf(stderr, "%s list '%s' does not contain %s\n", label, list, present);
		return 1;
	}
	if (strstr(list, absent)) {
		fprintf(stderr, "%s list '%s' unexpectedly contains %s\n", label, list, absent);
		return 1;
	}
	return 0;
}

static int messages_contain(char **messages, const char *regexp)
{
	if (!messages)
		return 0;
	while (*messages) {
		if (strcmp(*messages, regexp) == 0)
			return 1;
		messages++;
	}
	return 0;
}

static int expect_messages(IvyContext *ctx, IvyClientPtr app,
			   const char *present, const char *absent,
			   const char *label)
{
	char **messages = IvyContextGetApplicationMessages(ctx, app);

	if (!messages) {
		fprintf(stderr, "%s message list is NULL\n", label);
		return 1;
	}
	if (present && !messages_contain(messages, present)) {
		fprintf(stderr, "%s messages do not contain '%s'\n", label, present);
		return 1;
	}
	if (absent && messages_contain(messages, absent)) {
		fprintf(stderr, "%s messages unexpectedly contain '%s'\n", label, absent);
		return 1;
	}
	return 0;
}

static void message_callback(IvyClientPtr app, void *user_data, int argc, char *argv[])
{
	ReceiverState *state = (ReceiverState *)user_data;
	int i;

	(void)app;
	pthread_mutex_lock(&state->mutex);
	if (argc < 2) {
		state->malformed_count++;
		pthread_mutex_unlock(&state->mutex);
		return;
	}
	if (strcmp(argv[0], state->tag) != 0) {
		state->unexpected_tag_count++;
		pthread_mutex_unlock(&state->mutex);
		return;
	}
	for (i = 0; i < state->payload_count; i++) {
		if (strcmp(state->payloads[i].payload, argv[1]) == 0) {
			state->payloads[i].count++;
			pthread_mutex_unlock(&state->mutex);
			return;
		}
	}
	state->untracked_payload_count++;
	pthread_mutex_unlock(&state->mutex);
}

static void direct_callback(IvyClientPtr app, void *user_data, int id, char *msg)
{
	ReceiverState *state = (ReceiverState *)user_data;

	(void)app;
	pthread_mutex_lock(&state->mutex);
	if (id == state->direct_id && msg && strcmp(msg, state->direct_msg) == 0)
		state->direct_count++;
	else
		state->malformed_count++;
	pthread_mutex_unlock(&state->mutex);
}

static void bind_callback(IvyClientPtr app, void *user_data, int id,
			  const char *regexp, IvyBindEvent event)
{
	SenderState *state = (SenderState *)user_data;
	const char *app_name = IvyGetApplicationName(app);

	(void)id;
	(void)regexp;
	if (strcmp(app_name, state->peer_name) != 0)
		return;

	pthread_mutex_lock(&state->mutex);
	switch (event) {
	case IvyAddBind:
		state->add_count++;
		break;
	case IvyChangeBind:
		state->change_count++;
		break;
	case IvyRemoveBind:
		state->remove_count++;
		break;
	default:
		break;
	}
	pthread_mutex_unlock(&state->mutex);
}

static void pong_callback(IvyClientPtr app, int round_trip_delay)
{
	const char *app_name = IvyGetApplicationName(app);

	pthread_mutex_lock(&pong_state.mutex);
	if (round_trip_delay < 0) {
		pong_state.unexpected_count++;
	} else if (strcmp(app_name, pong_state.rx_a_name) == 0) {
		pong_state.pong_a_count++;
	} else if (strcmp(app_name, pong_state.rx_b_name) == 0) {
		pong_state.pong_b_count++;
	} else {
		pong_state.unexpected_count++;
	}
	pthread_mutex_unlock(&pong_state.mutex);
}

int main(int argc, char **argv)
{
	static const char initial_regexp[] = "^PHASE65 ([^ ]+) (.*)";
	static const char changed_regexp[] = "^PHASE65_CHANGED ([^ ]+) (.*)";
	const char *bus_a;
	const char *bus_b;
	const char *rx_a_name = "phase65-rx-a";
	const char *tx_a_name = "phase65-tx-a";
	const char *rx_b_name = "phase65-rx-b";
	const char *tx_b_name = "phase65-tx-b";
	ReceiverState rx_a_state;
	ReceiverState rx_b_state;
	SenderState tx_a_state;
	SenderState tx_b_state;
	ContextRun rx_a = { NULL, rx_a_name, 0, 0 };
	ContextRun tx_a = { NULL, tx_a_name, 0, 0 };
	ContextRun rx_b = { NULL, rx_b_name, 0, 0 };
	ContextRun tx_b = { NULL, tx_b_name, 0, 0 };
	MsgRcvPtr rx_a_bind = NULL;
	MsgRcvPtr rx_b_bind = NULL;
	IvyClientPtr app_rx_a;
	IvyClientPtr app_rx_b;
	int failed = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: %s BUS_A BUS_B\n", argv[0]);
		return 2;
	}

	bus_a = argv[1];
	bus_b = argv[2];

	receiver_init(&rx_a_state, "rx-a", "bus-a", "direct-a", 101);
	receiver_track(&rx_a_state, "first-a");
	receiver_track(&rx_a_state, "changed-a");
	receiver_track(&rx_a_state, "old-after-change-a");

	receiver_init(&rx_b_state, "rx-b", "bus-b", "direct-b", 202);
	receiver_track(&rx_b_state, "first-b");
	receiver_track(&rx_b_state, "after-unbind-b");
	receiver_track(&rx_b_state, "rebound-b");
	receiver_track(&rx_b_state, "final-b");

	sender_init(&tx_a_state, "tx-a", rx_a_name);
	sender_init(&tx_b_state, "tx-b", rx_b_name);
	pong_state_init(rx_a_name, rx_b_name);

	rx_a.ctx = IvyContextCreate(rx_a_name, NULL, NULL, NULL, NULL, NULL);
	tx_a.ctx = IvyContextCreate(tx_a_name, NULL, NULL, NULL, NULL, NULL);
	rx_b.ctx = IvyContextCreate(rx_b_name, NULL, NULL, NULL, NULL, NULL);
	tx_b.ctx = IvyContextCreate(tx_b_name, NULL, NULL, NULL, NULL, NULL);
	if (!rx_a.ctx || !tx_a.ctx || !rx_b.ctx || !tx_b.ctx) {
		fprintf(stderr, "IvyContextCreate failed, last error %d\n", IvyGetLastError());
		failed = 1;
		goto cleanup;
	}

	rx_a_bind = IvyContextBindMsg(rx_a.ctx, message_callback,
				      &rx_a_state, "%s", initial_regexp);
	rx_b_bind = IvyContextBindMsg(rx_b.ctx, message_callback,
				      &rx_b_state, "%s", initial_regexp);
	if (!rx_a_bind || !rx_b_bind) {
		fprintf(stderr, "IvyContextBindMsg failed, last error %d\n", IvyGetLastError());
		failed = 1;
		goto cleanup;
	}
	failed |= expect_status("rx-a direct bind",
				IvyContextBindDirectMsg(rx_a.ctx, direct_callback, &rx_a_state),
				IVY_OK);
	failed |= expect_status("rx-b direct bind",
				IvyContextBindDirectMsg(rx_b.ctx, direct_callback, &rx_b_state),
				IVY_OK);
	failed |= expect_status("tx-a bind callback",
				IvyContextSetBindCallback(tx_a.ctx, bind_callback, &tx_a_state),
				IVY_OK);
	failed |= expect_status("tx-b bind callback",
				IvyContextSetBindCallback(tx_b.ctx, bind_callback, &tx_b_state),
				IVY_OK);
	failed |= expect_status("tx-a pong callback",
				IvyContextSetPongCallback(tx_a.ctx, pong_callback),
				IVY_OK);
	failed |= expect_status("tx-b pong callback",
				IvyContextSetPongCallback(tx_b.ctx, pong_callback),
				IVY_OK);
	if (failed)
		goto cleanup;

	failed |= start_context(&rx_a, bus_a);
	failed |= start_context(&rx_b, bus_b);
	usleep(2 * WAIT_USEC);
	failed |= start_context(&tx_a, bus_a);
	failed |= start_context(&tx_b, bus_b);
	if (failed)
		goto cleanup;

	failed |= wait_sender_event(&tx_a_state, IvyAddBind, 1, "tx-a initial bind");
	failed |= wait_sender_event(&tx_b_state, IvyAddBind, 1, "tx-b initial bind");
	failed |= wait_application(tx_a.ctx, rx_a_name, "tx-a");
	failed |= wait_application(tx_b.ctx, rx_b_name, "tx-b");
	failed |= expect_no_application(tx_a.ctx, rx_b_name, "tx-a");
	failed |= expect_no_application(tx_b.ctx, rx_a_name, "tx-b");
	failed |= expect_application_list(tx_a.ctx, rx_a_name, rx_b_name, "tx-a");
	failed |= expect_application_list(tx_b.ctx, rx_b_name, rx_a_name, "tx-b");
	if (failed)
		goto cleanup;

	app_rx_a = IvyContextGetApplication(tx_a.ctx, (char *)rx_a_name);
	app_rx_b = IvyContextGetApplication(tx_b.ctx, (char *)rx_b_name);
	failed |= expect_messages(tx_a.ctx, app_rx_a, initial_regexp, changed_regexp,
				  "tx-a initial messages");
	failed |= expect_messages(tx_b.ctx, app_rx_b, initial_regexp, changed_regexp,
				  "tx-b initial messages");

	failed |= expect_positive_send("tx-a first",
		IvyContextSendMsg(tx_a.ctx, "PHASE65 bus-a first-a"));
	failed |= expect_positive_send("tx-b first",
		IvyContextSendMsg(tx_b.ctx, "PHASE65 bus-b first-b"));
	failed |= wait_payload(&rx_a_state, "first-a", 1, "rx-a");
	failed |= wait_payload(&rx_b_state, "first-b", 1, "rx-b");
	failed |= expect_no_unexpected(&rx_a_state);
	failed |= expect_no_unexpected(&rx_b_state);
	if (failed)
		goto cleanup;

	failed |= expect_status("tx-a ping", IvyContextSendPing(tx_a.ctx, app_rx_a), IVY_OK);
	failed |= expect_status("tx-b ping", IvyContextSendPing(tx_b.ctx, app_rx_b), IVY_OK);
	failed |= wait_pong_count(rx_a_name, 1, "tx-a");
	failed |= wait_pong_count(rx_b_name, 1, "tx-b");
	failed |= expect_status("tx-a direct",
		IvyContextSendDirectMsg(tx_a.ctx, app_rx_a, 101, "direct-a"), IVY_OK);
	failed |= expect_status("tx-b direct",
		IvyContextSendDirectMsg(tx_b.ctx, app_rx_b, 202, "direct-b"), IVY_OK);
	failed |= wait_direct(&rx_a_state, 1, "rx-a");
	failed |= wait_direct(&rx_b_state, 1, "rx-b");
	if (failed)
		goto cleanup;

	rx_a_bind = IvyContextChangeMsg(rx_a.ctx, rx_a_bind, "%s", changed_regexp);
	if (!rx_a_bind) {
		fprintf(stderr, "IvyContextChangeMsg failed with %d\n", IvyGetLastError());
		failed = 1;
		goto cleanup;
	}
	failed |= wait_sender_event(&tx_a_state, IvyChangeBind, 1, "tx-a change bind");
	failed |= expect_messages(tx_a.ctx, app_rx_a, changed_regexp, initial_regexp,
				  "tx-a changed messages");
	failed |= expect_zero_send("tx-a old after change",
		IvyContextSendMsg(tx_a.ctx, "PHASE65 bus-a old-after-change-a"));
	failed |= expect_no_payload_after_wait(&rx_a_state, "old-after-change-a", "rx-a");
	failed |= expect_positive_send("tx-a changed",
		IvyContextSendMsg(tx_a.ctx, "PHASE65_CHANGED bus-a changed-a"));
	failed |= wait_payload(&rx_a_state, "changed-a", 1, "rx-a");
	if (failed)
		goto cleanup;

	failed |= expect_status("rx-b unbind", IvyContextUnbindMsg(rx_b.ctx, rx_b_bind), IVY_OK);
	rx_b_bind = NULL;
	failed |= wait_sender_event(&tx_b_state, IvyRemoveBind, 1, "tx-b remove bind");
	failed |= expect_zero_send("tx-b after unbind",
		IvyContextSendMsg(tx_b.ctx, "PHASE65 bus-b after-unbind-b"));
	failed |= expect_no_payload_after_wait(&rx_b_state, "after-unbind-b", "rx-b");
	rx_b_bind = IvyContextBindMsg(rx_b.ctx, message_callback,
				      &rx_b_state, "%s", initial_regexp);
	if (!rx_b_bind) {
		fprintf(stderr, "rx-b rebind failed with %d\n", IvyGetLastError());
		failed = 1;
		goto cleanup;
	}
	failed |= wait_sender_event(&tx_b_state, IvyAddBind, 2, "tx-b rebind");
	failed |= expect_positive_send("tx-b rebound",
		IvyContextSendMsg(tx_b.ctx, "PHASE65 bus-b rebound-b"));
	failed |= wait_payload(&rx_b_state, "rebound-b", 1, "rx-b");
	if (failed)
		goto cleanup;

	failed |= stop_context(&tx_a);
	failed |= stop_context(&rx_a);
	failed |= destroy_context(&tx_a);
	failed |= destroy_context(&rx_a);
	if (failed)
		goto cleanup;

	failed |= expect_positive_send("tx-b final after bus-a stop",
		IvyContextSendMsg(tx_b.ctx, "PHASE65 bus-b final-b"));
	failed |= wait_payload(&rx_b_state, "final-b", 1, "rx-b");
	failed |= expect_no_unexpected(&rx_b_state);

cleanup:
	failed |= stop_context(&tx_a);
	failed |= stop_context(&rx_a);
	failed |= stop_context(&tx_b);
	failed |= stop_context(&rx_b);
	failed |= destroy_context(&tx_a);
	failed |= destroy_context(&rx_a);
	failed |= destroy_context(&tx_b);
	failed |= destroy_context(&rx_b);
	receiver_destroy(&rx_a_state);
	receiver_destroy(&rx_b_state);
	sender_destroy(&tx_a_state);
	sender_destroy(&tx_b_state);
	pong_state_destroy();
	return failed ? 1 : 0;
}
