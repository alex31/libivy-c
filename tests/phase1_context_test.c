#include "ivy.h"

#include <stdio.h>

extern int IvyLegacyDefaultContextIsInitialized(void);

int main(void)
{
	IvyContext *ctx;

	if (IvyLegacyDefaultContextIsInitialized()) {
		fprintf(stderr, "default context is initialized before any API call\n");
		return 1;
	}

	ctx = IvyContextCreate("context-only", "ready", NULL, NULL, NULL, NULL);
	if (ctx == NULL) {
		fprintf(stderr, "IvyContextCreate returned NULL\n");
		return 2;
	}

	if (IvyLegacyDefaultContextIsInitialized()) {
		fprintf(stderr, "IvyContextCreate initialized the legacy default context\n");
		return 3;
	}

	IvyContextDestroy(ctx);
	if (IvyLegacyDefaultContextIsInitialized()) {
		fprintf(stderr, "destroying a non-default context initialized default context\n");
		return 4;
	}

	IvyInit("legacy", NULL, NULL, NULL, NULL, NULL);
	if (!IvyLegacyDefaultContextIsInitialized()) {
		fprintf(stderr, "IvyInit did not initialize the legacy default context\n");
		return 5;
	}

	IvyTerminate();
	if (IvyLegacyDefaultContextIsInitialized()) {
		fprintf(stderr, "IvyTerminate did not clear the legacy default context\n");
		return 6;
	}

	return 0;
}
