#include "ivy.h"

#include <stdio.h>

static int expect_status(const char *label, int got, int expected)
{
	if (got != expected) {
		fprintf(stderr, "%s: got %d, expected %d\n", label, got, expected);
		return 1;
	}
	if (IvyGetLastError() != expected) {
		fprintf(stderr, "%s: last_error got %d, expected %d\n",
			label, IvyGetLastError(), expected);
		return 1;
	}
	return 0;
}

static int expect_state(const char *label, IvyContext *ctx, IvyContextState expected)
{
	IvyContextState got = IvyContextGetState(ctx);

	if (got != expected) {
		fprintf(stderr, "%s: got state %d, expected %d\n", label, got, expected);
		return 1;
	}
	if (IvyGetLastError() != IVY_OK) {
		fprintf(stderr, "%s: last_error got %d, expected %d\n",
			label, IvyGetLastError(), IVY_OK);
		return 1;
	}
	return 0;
}

int main(void)
{
	IvyContext *ctx;
	MsgRcvPtr msg;

	if (IvyGetLastError() != IVY_OK) {
		fprintf(stderr, "initial last_error is not IVY_OK\n");
		return 1;
	}

	if (expect_status("start null context", IvyContextStart(NULL, NULL), IVY_EINVAL))
		return 2;

	ctx = IvyContextCreate("phase2", "ready", NULL, NULL, NULL, NULL);
	if (ctx == NULL) {
		fprintf(stderr, "IvyContextCreate failed with %d\n", IvyGetLastError());
		return 3;
	}

	if (expect_state("new context", ctx, IVY_CTX_CREATED))
		return 4;
	if (expect_status("context stop", IvyContextStop(ctx), IVY_OK))
		return 5;
	if (expect_state("stopped context", ctx, IVY_CTX_STOPPED))
		return 6;
	if (expect_status("context stop idempotent", IvyContextStop(ctx), IVY_OK))
		return 7;
	if (expect_status("restart stopped context", IvyContextStart(ctx, NULL), IVY_ESTATE))
		return 8;
	if (expect_status("destroy stopped context", IvyContextDestroy(ctx), IVY_OK))
		return 9;

	if (expect_status("legacy init", IvyInit("legacy", NULL, NULL, NULL, NULL, NULL), IVY_OK))
		return 10;
	if (expect_status("legacy stop", IvyStop(), IVY_OK))
		return 11;
	if (expect_status("legacy stop idempotent", IvyStop(), IVY_OK))
		return 12;
	if (expect_status("legacy send after stop", IvySendMsg("after-stop"), IVY_ESTOPPED))
		return 13;

	msg = IvyBindMsg(NULL, NULL, "^test");
	if (msg != NULL) {
		fprintf(stderr, "IvyBindMsg after stop returned a binding\n");
		return 14;
	}
	if (IvyGetLastError() != IVY_ESTOPPED) {
		fprintf(stderr, "bind after stop last_error got %d, expected %d\n",
			IvyGetLastError(), IVY_ESTOPPED);
		return 15;
	}

	if (expect_status("legacy callback set after stop", IvySetPongCallback(NULL), IVY_ESTOPPED))
		return 16;
	if (expect_status("legacy start after stop", IvyStart(NULL), IVY_ESTATE))
		return 17;
	if (expect_status("legacy terminate", IvyTerminate(), IVY_OK))
		return 18;

	if (IvyContextGetState(NULL) != IVY_CTX_DESTROYED) {
		fprintf(stderr, "NULL context state should be IVY_CTX_DESTROYED\n");
		return 19;
	}
	if (IvyGetLastError() != IVY_EINVAL) {
		fprintf(stderr, "NULL context last_error got %d, expected %d\n",
			IvyGetLastError(), IVY_EINVAL);
		return 20;
	}

	return 0;
}
