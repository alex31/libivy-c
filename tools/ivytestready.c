/*
 *	Ivy perf mesure le temp de round trip
 *
 *	Copyright (C) 1997-2004
 *	Centre d'Études de la Navigation Aérienne
 *
 * 	Main and only file
 *
 *	Authors: François-Régis Colin <fcolin@cena.fr>
 * 	         Yannick Jestin <jestin@cena.fr>
 *
 *	Please refer to file version.h for the
 *	copyright notice regarding this software
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#include <windows.h>
#ifdef __MINGW32__
#include <regex.h> 
#include <getopt.h>
#endif
#else
#include <sys/time.h>
#include <unistd.h>
#ifdef __INTERIX
extern char *optarg;
extern int optind;
#endif
#endif


#include "ivysocket.h"
#include "ivy.h"
#include "timer.h"
#include "ivyloop.h"
#define MILLISEC 1000.0

const char * me = "A";
const char * other = "B";
char ready_message[1000] = "A ready";
char ready_bind[1000] = "^B ready";
static IvyContext *ready_ctx = NULL;

void Ready (IvyClientPtr app, void *user_data, int argc, char *argv[])
{
	const char *name = IvyContextGetApplicationName( ready_ctx, app );
	int count = IvyContextSendMsg (ready_ctx, "are you there %s",name);
	printf("Application %s received '%s' from %s sent question 'are you there %s'= %d\n", me, ready_bind, name, name, count);
}

void Question (IvyClientPtr app, void *user_data, int argc, char *argv[])
{
	const char *name = IvyContextGetApplicationName( ready_ctx, app );
	int count = IvyContextSendMsg (ready_ctx, "yes i am %s",me);
	printf("Application %s Reply to %s are you there = %d\n", me, name, count);
	
}
void Reply (IvyClientPtr app, void *user_data, int argc, char *argv[])
{
	const char *name = IvyContextGetApplicationName( ready_ctx, app );
	printf("Application %s Reply to our question! %s\n", name, argv[0]);
	
}

void binCB( IvyClientPtr app, void *user_data, int id, const char* regexp,  IvyBindEvent event ) 
{
	const char *app_name = IvyContextGetApplicationName( ready_ctx, app );
	switch ( event )
	{
	case IvyAddBind:
		printf("%s receive Application:%s bind '%s' ADDED\n", me, app_name, regexp );
		//if ( *me == 'A' ) usleep( 200000 ); // slowdown sending of regexp
		break;
	case IvyRemoveBind:
		printf("%s receive Application:%s bind '%s' REMOVED\n", me, app_name, regexp );
		break;
	case IvyChangeBind:
		printf("%s receive Application:%s bind '%s' CHANGED\n", me, app_name, regexp );
		break;
	case IvyFilterBind:
		printf("%s receive Application:%s bind '%s' FILTRED\n", me, app_name, regexp );
		break;

	}
}




int main(int argc, char *argv[])
{
	
	/* Mainloop management */
	if ( argc > 1 )
	{
	 me = "B" ;
	 other = "A";
	 strcpy( ready_message, "B ready");
	 strcpy( ready_bind, "^A ready");
	}

	ready_ctx = IvyContextCreate (me, ready_message, NULL,NULL,NULL,NULL);
	if (ready_ctx == NULL) {
		fprintf(stderr, "IvyContextCreate failed: %d\n", IvyGetLastError());
		return 1;
	}
	IvyContextSetBindCallback( ready_ctx, binCB, 0 );

#if defined(__GNUC__) && __GNUC_PREREQ(4,7)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#endif
	IvyContextBindMsg (ready_ctx, Ready, NULL, ready_bind);
#if defined(__GNUC__) && __GNUC_PREREQ(4,7)
#pragma GCC diagnostic pop
#endif

	IvyContextBindMsg (ready_ctx, Question, NULL, "^are you there %s",me);
	IvyContextBindMsg (ready_ctx, Reply, NULL, "^(yes i am %s)",other);
	 
	if (IvyContextStart (ready_ctx, NULL) != IVY_OK) {
		fprintf(stderr, "IvyContextStart failed: %d\n", IvyGetLastError());
		IvyContextDestroy(ready_ctx);
		return 1;
	}

	
	IvyContextMainLoop (ready_ctx);
	IvyContextDestroy(ready_ctx);
	return 0;
}
