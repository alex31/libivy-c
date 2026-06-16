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

#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#include <windows.h>
#include "getopt.h"
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

const char *mymessages[] = { "IvyPerf", "ping", "pong" };
static double origin = 0;
long nbMsgReceive=0;
long nbMsgEmit=0;
long nbMsg = 0;
static IvyContext *perf_ctx = NULL;
static int loop_sender = 0;
static long send_period = 200;
static TimerId send_timer = 0;
static int send_timer_running = 0;


double minRoundTrip=1e12;
double maxRoundTrip=0;
double averageRoundTrip=0;

static double currentTime()
{
        double current;
#ifdef WIN32
        current = GetTickCount();
#else
        struct timeval stamp;
        gettimeofday( &stamp, NULL );
        current = (double)stamp.tv_sec * MILLISEC + (double)(stamp.tv_usec/MILLISEC);
#endif
        return  current;
}

static void TimerCall(TimerId id, void *user_data, unsigned long delta);

static void ResetRoundTripStats(void)
{
	nbMsgReceive = 0;
	nbMsgEmit = 0;
	minRoundTrip = 1e12;
	maxRoundTrip = 0;
	averageRoundTrip = 0;
}

static void StartSenderTimer(IvyContext *ctx)
{
	send_timer = IvyContextTimerRepeatAfter(ctx, TIMER_LOOP, send_period,
					       TimerCall, ctx);
	send_timer_running = 1;
}

static void StopSenderTimer(void)
{
	if (!send_timer_running)
		return;

	TimerRemove(send_timer);
	send_timer_running = 0;
	send_timer = 0;
}

static void PrintUsage(const char *program)
{
	fprintf(stderr,
		"usage: %s [-l] [-b bus] [period_ms [message_count]]\n"
		"       no period/count runs as a passive responder\n"
		"       message_count defaults to 10 when period_ms is supplied\n"
		"       -l repeats sender batches instead of exiting\n",
		program);
}


void Reply (IvyClientPtr app, void *user_data, int argc, char *argv[])
{
	IvyContext *ctx = (IvyContext *)user_data;
	IvyContextSendMsg (ctx, "pong ts=%s tr=%f", *argv, currentTime()- origin);
}
void Pong (IvyClientPtr app, void *user_data, int argc, char *argv[])
{
	double current;
	double ts;
	double roundtrip3;

	if ( nbMsg <= 0 )
		return;

	nbMsgReceive++;
	
	current = currentTime() - origin ;
	ts = atof( *argv++ );
	roundtrip3 = current - ts;
	if ( roundtrip3 > maxRoundTrip ) maxRoundTrip = roundtrip3;
	if ( roundtrip3 < minRoundTrip ) minRoundTrip = roundtrip3;
	averageRoundTrip = (averageRoundTrip * ( nbMsgReceive - 1 ) + roundtrip3) /nbMsgReceive;
	
	if ( nbMsg == nbMsgReceive )
	{
		printf("roundtrip[%ld] min %f av %f max %f ms\n", nbMsgReceive, minRoundTrip, averageRoundTrip, maxRoundTrip );
		fflush(stdout);
		if ( loop_sender ) {
			ResetRoundTripStats();
			StartSenderTimer(perf_ctx);
		} else {
			IvyContextStop(perf_ctx);
		}
	}

}

static void TimerCall(TimerId id, void *user_data, unsigned long delta)
{
	IvyContext *ctx = (IvyContext *)user_data;
	int count = IvyContextSendMsg (ctx, "ping ts=%f", currentTime() - origin );
	if ( count ) nbMsgEmit++;
	if ( nbMsg == nbMsgEmit )
		{
		StopSenderTimer();
		}
}

void binCB( IvyClientPtr app, void *user_data, int id, const char* regexp,  IvyBindEvent event ) 
{
	const char *app_name = IvyContextGetApplicationName( perf_ctx, app );
	switch ( event )
	{
	case IvyAddBind:
		printf("Application:%s bind '%s' ADDED\n", app_name, regexp );
		break;
	case IvyRemoveBind:
		printf("Application:%s bind '%s' REMOVED\n", app_name, regexp );
		break;
	case IvyChangeBind:
		printf("Application:%s bind '%s' CHANGED\n", app_name, regexp );
		break;
	case IvyFilterBind:
		printf("Application:%s bind '%s' FILTRED\n", app_name, regexp );
		break;

	}
}
int main(int argc, char *argv[])
{
	int c;
	int sender_mode = 0;
	const char * bus = NULL;
	while ((c = getopt(argc, argv, "b:lh")) != EOF)
			switch (c) {
			case 'b':
				bus = optarg;
				break;
			case 'l':
				loop_sender = 1;
				break;
			case 'h':
				PrintUsage(argv[0]);
				return 0;
			default:
				PrintUsage(argv[0]);
				return 1;
	}
	/* Mainloop management */
	if ( optind < argc ) {
		sender_mode = 1;
		send_period = atol( argv[optind++] );
		nbMsg = 10;
		if ( optind < argc )
			nbMsg = atol( argv[optind++] );
	}
	if ( optind < argc || send_period <= 0 ||
	     (sender_mode && nbMsg <= 0) ) {
		PrintUsage(argv[0]);
		return 1;
	}

	perf_ctx = IvyContextCreate ("IvyPerf", "IvyPerf ready", NULL,NULL,NULL,NULL);
	if (perf_ctx == NULL) {
		fprintf(stderr, "IvyContextCreate failed: %d\n", IvyGetLastError());
		return 1;
	}
	IvySetFilter( sizeof( mymessages )/ sizeof( char *),mymessages );
	IvyContextSetBindCallback( perf_ctx, binCB, 0 );
	IvyContextBindMsg (perf_ctx, Reply, perf_ctx, "^ping ts=(.*)");
	IvyContextBindMsg (perf_ctx, Pong, NULL, "^pong ts=(.*) tr=(.*)");
	  
	origin = currentTime();
	if (IvyContextStart (perf_ctx, bus) != IVY_OK) {
		fprintf(stderr, "IvyContextStart failed: %d\n", IvyGetLastError());
		IvyContextDestroy(perf_ctx);
		return 1;
	}

	if ( nbMsg > 0 ) {
		printf("sender: %ld pings every %ld ms%s, first report after about %.1f s\n",
		       nbMsg, send_period, loop_sender ? " in a loop" : "",
		       ((double)nbMsg * (double)send_period) / MILLISEC);
		fflush(stdout);
		StartSenderTimer(perf_ctx);
	} else {
		printf("responder: waiting for pings\n");
		fflush(stdout);
	}
	

	IvyContextMainLoop (perf_ctx);
	IvyContextDestroy(perf_ctx);
	return 0;
}
