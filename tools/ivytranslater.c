#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <ivy.h>

static IvyContext *translater_ctx = NULL;

/* callback associated to "Hello" messages */
void HelloCallback (IvyClientPtr app, void *data, int argc, char **argv)
{
	const char* arg = (argc < 1) ? "" : argv[0];
	IvyContextSendMsg (translater_ctx, "Bonjour%s", arg);
}

/* callback associated to "Bye" messages */
void ByeCallback (IvyClientPtr app, void *data, int argc, char **argv)
{
	IvyContextStop (translater_ctx);
}

int main (int argc, char**argv)
{
	/* handling of -b option */
	const char* bus = 0;
	int c;
	while ((c = getopt (argc, argv, "b:")) != EOF) {
		switch (c) {
		case 'b':
			bus = optarg;
			break;
		}
	}

	/* handling of environment variable */
	if (!bus)
		bus = getenv ("IVYBUS");

	/* initializations */
	translater_ctx = IvyContextCreate ("IvyTranslater", "Hello le monde",
					   NULL, NULL, NULL, NULL);
	if (translater_ctx == NULL) {
		fprintf(stderr, "IvyContextCreate failed: %d\n", IvyGetLastError());
		return 1;
	}

	/* binding of HelloCallback to messages starting with 'Hello' */
	IvyContextBindMsg (translater_ctx, HelloCallback, 0, "^Hello(.*)");

	/* binding of ByeCallback to 'Bye' */
	IvyContextBindMsg (translater_ctx, ByeCallback, 0, "^Bye$");

	if (IvyContextStart (translater_ctx, bus) != IVY_OK) {
		fprintf(stderr, "IvyContextStart failed: %d\n", IvyGetLastError());
		IvyContextDestroy(translater_ctx);
		return 1;
	}

	/* main loop */
	IvyContextMainLoop(translater_ctx);
	IvyContextDestroy(translater_ctx);
	return 0;
}
