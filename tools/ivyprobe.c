/*
 *	Ivy probe
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
#define DEFAULT_IVYPROBE_NAME "IVYPROBE"
#define DEFAULT_READY " Ready"
#include "version.h"

#define IVYMAINLOOP

#ifdef XTMAINLOOP
#undef IVYMAINLOOP
#endif
#ifdef GLIBMAINLOOP
#undef IVYMAINLOOP
#endif

#ifdef GLUTMAINLOOP
#undef IVYMAINLOOP
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#include <windows.h>
#ifdef __MINGW32__
#include <regex.h> 
#include <getopt.h>
#else
#include "getopt.h"
#endif
#else
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __INTERIX
extern char *optarg;
extern int optind;
#endif

#endif
#ifdef XTMAINLOOP
#include "ivyxtloop.h"
#endif
#ifdef GLIBMAINLOOP
#include <glib.h>
#include "ivyglibloop.h"
#endif
#ifdef GLUTMAINLOOP
#include "ivyglutloop.h"
#endif
#ifdef IVYMAINLOOP
#include "ivyloop.h"
#endif
#include "ivysocket.h"
#include "ivychannel.h"
#include "ivybind.h" /* to test regexp before passing to BinMsg */
#include "ivy.h"
#include "timer.h"
#ifdef XTMAINLOOP
#include <X11/Intrinsic.h>
XtAppContext cntx;
#endif

int app_count = 0;
int wait_count = 0;
int fbindcallback = 0;
int filter_count = 0;
const char *filter[4096];
char *classes;

typedef struct ProbeBus {
	IvyContext *ctx;
	char *bus;
	int app_count;
#ifndef WIN32
	pthread_t loop_thread;
	int loop_started;
#endif
} ProbeBus;

static ProbeBus *probe_buses = NULL;
static size_t probe_bus_count = 0;
static size_t probe_bus_capacity = 0;
static int probe_running = 1;

#ifndef WIN32
static pthread_mutex_t probe_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t probe_state_cond = PTHREAD_COND_INITIALIZER;
#endif

#define PROBE_MAX_THREADS 10

#ifndef WIN32
typedef struct ProbeCommand {
	struct ProbeCommand *next;
	char *line;
} ProbeCommand;

typedef struct {
	int id;
	int started;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	ProbeCommand *head;
	ProbeCommand *tail;
	unsigned int queue_size;
} ProbeWorker;

static ProbeWorker probe_workers[PROBE_MAX_THREADS];
#endif

static int current_probe_thread = 0;

void DirectCallback(IvyClientPtr app, void *user_data, int id, char *msg);
void PongCallback(IvyClientPtr app, int roundTripOrTimout);
void Callback(IvyClientPtr app, void *user_data, int argc, char *argv[]);
void ApplicationCallback(IvyClientPtr app, void *user_data, IvyApplicationEvent event);
void IvyPrintBindCallback(IvyClientPtr app, void *user_data, int id, const char* regexp, IvyBindEvent event);
static void ExecuteProbeCommand(char *line);
static void DispatchProbeCommand(char *line);

static char *ProbeStrtok(char *str, const char *delim, char **saveptr)
{
#ifdef WIN32
	(void)saveptr;
	return strtok(str, delim);
#else
	return strtok_r(str, delim, saveptr);
#endif
}

static const char *ProbeBusLabel(const ProbeBus *bus)
{
	if (!bus || !bus->bus || !bus->bus[0])
		return "default";
	return bus->bus;
}

static int ProbeAddBus(const char *bus)
{
	ProbeBus *new_buses;

	if (probe_bus_count == probe_bus_capacity) {
		size_t new_capacity = probe_bus_capacity ? probe_bus_capacity * 2 : 4;
		new_buses = (ProbeBus *)realloc(probe_buses, new_capacity * sizeof(*probe_buses));
		if (!new_buses)
			return 0;
		memset(new_buses + probe_bus_capacity, 0,
		       (new_capacity - probe_bus_capacity) * sizeof(*probe_buses));
		probe_buses = new_buses;
		probe_bus_capacity = new_capacity;
	}

	if (bus && bus[0]) {
		probe_buses[probe_bus_count].bus = strdup(bus);
		if (!probe_buses[probe_bus_count].bus)
			return 0;
	}
	probe_bus_count++;
	return 1;
}

static void ProbeAddConfiguredBuses(void)
{
	const char *env_bus = getenv("IVYBUS");

	if (env_bus && env_bus[0]) {
		if (!ProbeAddBus(env_bus)) {
			fprintf(stderr, "unable to add IVYBUS\n");
			exit(1);
		}
	}

	if (probe_bus_count == 0) {
		if (!ProbeAddBus(NULL)) {
			fprintf(stderr, "unable to add default bus\n");
			exit(1);
		}
	}
}

static int ProbeSendMsgAll(const char *message)
{
	size_t i;
	int total = 0;

	for (i = 0; i < probe_bus_count; i++) {
		int sent = IvyContextSendMsg(probe_buses[i].ctx, "%s", message);
		if (sent > 0)
			total += sent;
		else if (sent < 0)
			printf("send on %s failed with %d\n", ProbeBusLabel(&probe_buses[i]), sent);
	}
	return total;
}

static void ProbeBindAll(const char *regexp)
{
	size_t i;

	for (i = 0; i < probe_bus_count; i++)
		IvyContextBindMsg(probe_buses[i].ctx, Callback, &probe_buses[i], "%s", regexp);
}

static void ProbeSetBindCallbackAll(IvyBindCallback callback)
{
	size_t i;

	for (i = 0; i < probe_bus_count; i++)
		IvyContextSetBindCallback(probe_buses[i].ctx, callback, NULL);
}

#ifndef WIN32
static void *ProbeBusLoopMain(void *data)
{
	ProbeBus *bus = (ProbeBus *)data;
	IvyContextMainLoop(bus->ctx);
	return NULL;
}
#endif

static int ProbeCreateBuses(const char *agentname, const char *agentready)
{
	size_t i;

	for (i = 0; i < probe_bus_count; i++) {
		ProbeBus *bus = &probe_buses[i];

		bus->ctx = IvyContextCreate(agentname, agentready,
					    ApplicationCallback, bus, NULL, NULL);
		if (!bus->ctx) {
			fprintf(stderr, "IvyContextCreate failed for %s with %d\n",
				ProbeBusLabel(bus), IvyGetLastError());
			return 0;
		}
		IvyContextSetBindCallback(bus->ctx, IvyPrintBindCallback, bus);
		IvyContextSetPongCallback(bus->ctx, PongCallback);
		IvyContextBindDirectMsg(bus->ctx, DirectCallback, bus);
	}
	return 1;
}

static int ProbeStartBuses(void)
{
	size_t i;

	for (i = 0; i < probe_bus_count; i++) {
		ProbeBus *bus = &probe_buses[i];

		if (IvyContextStart(bus->ctx, bus->bus) != IVY_OK) {
			fprintf(stderr, "IvyContextStart failed for %s with %d\n",
				ProbeBusLabel(bus), IvyGetLastError());
			return 0;
		}
#ifndef WIN32
		if (pthread_create(&bus->loop_thread, NULL, ProbeBusLoopMain, bus) != 0) {
			fprintf(stderr, "unable to start loop thread for %s\n", ProbeBusLabel(bus));
			return 0;
		}
		bus->loop_started = 1;
#endif
	}
	return 1;
}

static void ProbeStopBuses(void)
{
	size_t i;

	probe_running = 0;
#ifndef WIN32
	pthread_mutex_lock(&probe_state_mutex);
	pthread_cond_broadcast(&probe_state_cond);
	pthread_mutex_unlock(&probe_state_mutex);
#endif
	for (i = 0; i < probe_bus_count; i++) {
		if (probe_buses[i].ctx)
			IvyContextStop(probe_buses[i].ctx);
	}
}

static void ProbeJoinBuses(void)
{
#ifndef WIN32
	size_t i;

	for (i = 0; i < probe_bus_count; i++) {
		if (probe_buses[i].loop_started) {
			pthread_join(probe_buses[i].loop_thread, NULL);
			probe_buses[i].loop_started = 0;
		}
	}
#endif
}

static void ProbeDestroyBuses(void)
{
	size_t i;

	for (i = 0; i < probe_bus_count; i++) {
		if (probe_buses[i].ctx) {
			IvyContextDestroy(probe_buses[i].ctx);
			probe_buses[i].ctx = NULL;
		}
		free(probe_buses[i].bus);
		probe_buses[i].bus = NULL;
	}
	free(probe_buses);
	probe_buses = NULL;
	probe_bus_count = 0;
	probe_bus_capacity = 0;
}

static void ProbeWaitForApplications(void)
{
#ifndef WIN32
	pthread_mutex_lock(&probe_state_mutex);
	while (probe_running && wait_count > 0 && app_count < wait_count)
		pthread_cond_wait(&probe_state_cond, &probe_state_mutex);
	pthread_mutex_unlock(&probe_state_mutex);
#endif
}

void DirectCallback(IvyClientPtr app, void *user_data, int id, char *msg ) 
{
	ProbeBus *bus = (ProbeBus *)user_data;

	if (probe_bus_count > 1)
		printf("[%s] %s sent a direct message, id=%d, message=%s\n",
		       ProbeBusLabel(bus), IvyGetApplicationName(app), id, msg);
	else
		printf("%s sent a direct message, id=%d, message=%s\n",
		       IvyGetApplicationName(app), id, msg);
}


void PongCallback (IvyClientPtr app, int roundTripOrTimout)
{
	if (roundTripOrTimout >= 0) {
	  printf ("%s respond to ping in %.3f ms\n", IvyGetApplicationName(app),
		  roundTripOrTimout/1000.0);
	} else {
	  printf ("%s ping timout after %.3f ms\n", IvyGetApplicationName(app),
		  -roundTripOrTimout/1000.0);
	}
}

void Callback (IvyClientPtr app, void *user_data, int argc, char *argv[])
{
	ProbeBus *bus = (ProbeBus *)user_data;
	int i;
	if (probe_bus_count > 1)
		printf ("[%s] %s sent ", ProbeBusLabel(bus), IvyGetApplicationName(app));
	else
		printf ("%s sent ",IvyGetApplicationName(app));
	for  (i = 0; i < argc; i++)
			printf(" '%s'",argv[i]);
	printf("\n");
}

char * Chop(char *arg)
{
  	size_t len;
	if (arg==NULL) return arg;
	len=strlen(arg)-1;
	if ((*(arg+len))=='\n') *(arg+len)=0;
	return arg;
}

#ifndef WIN32
static void *ProbeWorkerMain(void *data)
{
	ProbeWorker *worker = (ProbeWorker *)data;

	for (;;) {
		ProbeCommand *command;

		pthread_mutex_lock(&worker->mutex);
		while (worker->head == NULL)
			pthread_cond_wait(&worker->cond, &worker->mutex);

		command = worker->head;
		worker->head = command->next;
		if (worker->head == NULL)
			worker->tail = NULL;
		worker->queue_size--;
		pthread_mutex_unlock(&worker->mutex);

		ExecuteProbeCommand(command->line);
		free(command->line);
		free(command);
	}
	return NULL;
}

static int ProbeEnsureWorker(int id)
{
	ProbeWorker *worker;

	if (id <= 0 || id >= PROBE_MAX_THREADS)
		return 0;

	worker = &probe_workers[id];
	if (worker->started)
		return 1;

	worker->id = id;
	if (pthread_mutex_init(&worker->mutex, NULL) != 0)
		return 0;
	if (pthread_cond_init(&worker->cond, NULL) != 0) {
		pthread_mutex_destroy(&worker->mutex);
		return 0;
	}
	if (pthread_create(&worker->thread, NULL, ProbeWorkerMain, worker) != 0) {
		pthread_cond_destroy(&worker->cond);
		pthread_mutex_destroy(&worker->mutex);
		return 0;
	}
	pthread_detach(worker->thread);
	worker->started = 1;
	return 1;
}

static int ProbeQueueWorkerCommand(int id, const char *line)
{
	ProbeWorker *worker;
	ProbeCommand *command;

	if (!ProbeEnsureWorker(id))
		return 0;

	command = (ProbeCommand *)calloc(1, sizeof(*command));
	if (!command)
		return 0;
	command->line = strdup(line);
	if (!command->line) {
		free(command);
		return 0;
	}

	worker = &probe_workers[id];
	pthread_mutex_lock(&worker->mutex);
	if (worker->tail)
		worker->tail->next = command;
	else
		worker->head = command;
	worker->tail = command;
	worker->queue_size++;
	pthread_cond_signal(&worker->cond);
	pthread_mutex_unlock(&worker->mutex);
	return 1;
}
#endif

static int ProbeLineCommandIs(const char *line, const char *command)
{
	size_t len;

	if (!line || line[0] != '.')
		return 0;

	len = strlen(command);
	if (strncmp(line + 1, command, len) != 0)
		return 0;

	return line[1 + len] == '\0' ||
	       line[1 + len] == '\n' ||
	       line[1 + len] == ' ' ||
	       line[1 + len] == '\t' ||
	       line[1 + len] == ':';
}

static void ProbePrintCurrentThread(void)
{
	if (current_probe_thread == 0)
		printf("Current command thread: 0 (loop)\n");
	else
		printf("Current command thread: %d\n", current_probe_thread);
}

static void ProbePrintThreads(void)
{
	int id;

	printf("Thread 0: loop%s\n", current_probe_thread == 0 ? " *" : "");
	for (id = 1; id < PROBE_MAX_THREADS; id++) {
#ifndef WIN32
		ProbeWorker *worker = &probe_workers[id];
		if (worker->started) {
			unsigned int queue_size;
			pthread_mutex_lock(&worker->mutex);
			queue_size = worker->queue_size;
			pthread_mutex_unlock(&worker->mutex);
			printf("Thread %d: worker queue=%u%s\n",
			       id, queue_size, current_probe_thread == id ? " *" : "");
		}
#endif
	}
}

static void ProbeHandleThreadCommand(char *line)
{
	char *saveptr = NULL;
	char *cmd;
	char *arg;
	int id;

	cmd = ProbeStrtok(line, ".: \t\n", &saveptr);
	(void)cmd;
	arg = ProbeStrtok(NULL, " \t\n", &saveptr);
	if (!arg) {
		ProbePrintCurrentThread();
		return;
	}

	if (strcmp(arg, "loop") == 0) {
		current_probe_thread = 0;
		ProbePrintCurrentThread();
		return;
	}

	if (arg[0] < '0' || arg[0] > '9' || arg[1] != '\0') {
		printf(".thread expects loop or a value from 0 to 9\n");
		return;
	}

	id = arg[0] - '0';
	if (id > 0) {
#ifdef WIN32
		printf("Worker command threads are not available on Windows ivyprobe\n");
		return;
#else
		if (!ProbeEnsureWorker(id)) {
			printf("Unable to start command thread %d\n", id);
			return;
		}
#endif
	}
	current_probe_thread = id;
	ProbePrintCurrentThread();
}

static void DispatchProbeCommand(char *line)
{
	if (ProbeLineCommandIs(line, "thread")) {
		char copy[4096];
		snprintf(copy, sizeof(copy), "%s", line);
		ProbeHandleThreadCommand(copy);
		return;
	}

	if (ProbeLineCommandIs(line, "threads")) {
		ProbePrintThreads();
		return;
	}

	if (ProbeLineCommandIs(line, "quit")) {
		ExecuteProbeCommand(line);
		return;
	}

	if (current_probe_thread == 0) {
		ExecuteProbeCommand(line);
		return;
	}

#ifdef WIN32
	ExecuteProbeCommand(line);
#else
	if (!ProbeQueueWorkerCommand(current_probe_thread, line))
		printf("Unable to queue command on thread %d\n", current_probe_thread);
#endif
}

static void ExecuteProbeCommand(char *line)
{
	static const char *separator = "#";
	char *saveptr = NULL;
	char *list_saveptr = NULL;
	char *cmd;
	char *arg;
	int id;
	IvyClientPtr app;
	int err;
	if  (*line == '.') {
		cmd = ProbeStrtok(line, ".: \t\n", &saveptr);
		if (!cmd)
			return;

		if  (strcmp (cmd, "die") == 0) {
			arg = ProbeStrtok(NULL, " \t\n", &saveptr);
			if  (arg) {
				size_t i;
				int found = 0;
				for (i = 0; i < probe_bus_count; i++) {
					app = IvyContextGetApplication(probe_buses[i].ctx, arg);
					if (app) {
						IvyContextSendDieMsg(probe_buses[i].ctx, app);
						found++;
					}
				}
				if (!found)
					printf ("No Application %s!!!\n",arg);
			}

		} else if (strcmp(cmd, "dieall-yes-i-am-sure") == 0) {
			size_t i;
			for (i = 0; i < probe_bus_count; i++) {
				list_saveptr = NULL;
				arg = IvyContextGetApplicationList(probe_buses[i].ctx, separator);
				arg = ProbeStrtok(arg, separator, &list_saveptr);
				while  (arg) {
					app = IvyContextGetApplication(probe_buses[i].ctx, arg);
					if  (app)
					{
						printf ("Killing '%s'...\n",arg);
						IvyContextSendDieMsg(probe_buses[i].ctx, app);
					}
					else
						printf ("No Application %s!!!\n",arg);
					arg = ProbeStrtok(NULL, separator, &list_saveptr);
				}
			}

		} else if (strcmp(cmd,  "bind") == 0) {
		  arg = ProbeStrtok(NULL, "'", &saveptr);
		  Chop(arg);
		  if  (arg) {
		    IvyBinding binding;
		    const char *errbuf;
		    int erroffset;
		    binding = IvyBindingCompile(arg, & erroffset, & errbuf);
		    if (binding==NULL) {
			  printf("Error compiling '%s', %s, not bound\n", arg, errbuf);
		    } else {
			  IvyBindingFree( binding );
			  ProbeBindAll(arg);
		    }
		  }

		} else if  (strcmp(cmd,  "where") == 0) {
			arg = ProbeStrtok(NULL, " \t\n", &saveptr);
			if  (arg) {
				size_t i;
				int found = 0;
				for (i = 0; i < probe_bus_count; i++) {
					app = IvyContextGetApplication(probe_buses[i].ctx, arg);
					if (app) {
						if (probe_bus_count > 1)
							printf ("Application %s on %s via %s\n",
								arg, IvyGetApplicationHost(app),
								ProbeBusLabel(&probe_buses[i]));
						else
							printf ("Application %s on %s\n",
								arg, IvyGetApplicationHost(app));
						found++;
					}
				}
				if (!found)
					printf ("No Application %s!!!\n",arg);
			}
		} else if  (strcmp(cmd, "direct") == 0) {
			arg = ProbeStrtok(NULL, " \t\n", &saveptr);
			if  (arg) {
				char *target = arg;
				size_t i;
				int found = 0;
				arg = ProbeStrtok(NULL, " ", &saveptr);
				id = arg ? atoi (arg) : 0;
				arg = ProbeStrtok(NULL, "'", &saveptr);
				for (i = 0; i < probe_bus_count; i++) {
					app = IvyContextGetApplication(probe_buses[i].ctx, target);
					if (app) {
						IvyContextSendDirectMsg(probe_buses[i].ctx, app, id, Chop(arg));
						found++;
					}
				}
				if (!found)
					printf ("No Application %s!!!\n",target);
			}

		} else if  (strcmp(cmd, "who") == 0) {
			size_t i;
			for (i = 0; i < probe_bus_count; i++) {
				if (probe_bus_count > 1)
					printf("Apps[%s]: %s\n", ProbeBusLabel(&probe_buses[i]),
					       IvyContextGetApplicationList(probe_buses[i].ctx, ","));
				else
					printf("Apps: %s\n",
					       IvyContextGetApplicationList(probe_buses[i].ctx, ","));
			}

		} else if  (strcmp(cmd, "ping") == 0) {
		  arg = ProbeStrtok(NULL, " \t\n", &saveptr);
		  if  (arg) {
		    size_t i;
		    int found = 0;
		    for (i = 0; i < probe_bus_count; i++) {
		      app = IvyContextGetApplication(probe_buses[i].ctx, arg);
		      if (app) {
		        IvyContextSendPing(probe_buses[i].ctx, app);
		        found++;
		      }
		    }
		    if (!found)
		      printf ("No Application %s!!!\n",arg);
		  }
		} else if  (strcmp(cmd, "help") == 0) {
			fprintf(stderr,"Commands list:\n");
			printf("	.help						- this help\n");
			printf("	.quit						- terminate this application\n");
			printf("	.die appname				- send die msg to appname\n");
			printf("	.dieall-yes-i-am-sure		- send die msg to all applis\n");
			printf("	.direct appname	id 'arg'	- send direct msg to appname\n");
			printf("	.ping appname	                - send ping to appname\n");
			printf("	.where appname				- on which host is appname\n");
			printf("	.bind 'regexp'				- add a msg to receive\n");
			printf("	.showbind					- show bindings \n");
			printf("	.thread [0-9|loop]			- select command execution thread, 0 is loop\n");
			printf("	.threads					- list command threads\n");
			
			printf("	.who				- who is on the bus\n");
		} else if  (strcmp(cmd, "showbind") == 0) {
		  if (!fbindcallback) {
		    ProbeSetBindCallbackAll(IvyDefaultBindCallback);
		    fbindcallback=1;
		  } else {
		    ProbeSetBindCallbackAll(NULL);
		    fbindcallback=0;
		  }
		} else if  (strcmp(cmd, "quit") == 0) {
			ProbeStopBuses();
		}
	} else {
		cmd = ProbeStrtok(line, "\n", &saveptr);
		err = ProbeSendMsgAll(cmd);
		printf("-> Sent to %d peer%s\n", err, err == 1 ? "" : "s");
	}
}

void HandleStdin (Channel channel, IVY_HANDLE fd, void *data)
{
	char buf[4096];
	char *line;

	(void)fd;
	(void)data;

	line = fgets(buf, 4096, stdin);
	if  (!line)	{
		IvyChannelRemove (channel);
		ProbeStopBuses();
		return;
	}
	DispatchProbeCommand(line);
}

void ApplicationCallback (IvyClientPtr app, void *user_data, IvyApplicationEvent event)
{
	ProbeBus *bus = (ProbeBus *)user_data;
	const char *appname;
	const char *host;
/*	char **msgList;*/
	appname = IvyGetApplicationName (app);
	host = IvyGetApplicationHost (app);
	switch  (event)  {

	case IvyApplicationConnected:
#ifndef WIN32
		pthread_mutex_lock(&probe_state_mutex);
#endif
		app_count++;
		if (bus)
			bus->app_count++;
#ifndef WIN32
		pthread_cond_broadcast(&probe_state_cond);
		pthread_mutex_unlock(&probe_state_mutex);
#endif
		if (probe_bus_count > 1)
			printf("[%s] %s connected from %s\n", ProbeBusLabel(bus), appname,  host);
		else
			printf("%s connected from %s\n", appname,  host);
/*		printf("Application(%s): Begin Messages\n", appname);*/
/* double usage with -s flag remove it 
		msgList = IvyGetApplicationMessages (app);
		while (*msgList )
			printf("%s subscribes to '%s'\n",appname,*msgList++);
*/
/*		printf("Application(%s): End Messages\n",appname);*/
		break;

	case IvyApplicationDisconnected:
#ifndef WIN32
		pthread_mutex_lock(&probe_state_mutex);
#endif
		app_count--;
		if (bus)
			bus->app_count--;
#ifndef WIN32
		pthread_cond_broadcast(&probe_state_cond);
		pthread_mutex_unlock(&probe_state_mutex);
#endif
		if (probe_bus_count > 1)
			printf("[%s] %s disconnected from %s\n", ProbeBusLabel(bus), appname,  host);
		else
			printf("%s disconnected from %s\n", appname,  host);
		break;

	default:
		printf("%s: unkown event %d\n", appname, event);
		break;
	}
}
void IvyPrintBindCallback( IvyClientPtr app, void *user_data, int id, const char* regexp,  IvyBindEvent event)
{
	ProbeBus *bus = (ProbeBus *)user_data;
	const char *prefix = "";
	char prefix_buffer[256];

	if (probe_bus_count > 1) {
		snprintf(prefix_buffer, sizeof(prefix_buffer), "[%s] ", ProbeBusLabel(bus));
		prefix = prefix_buffer;
	}

        switch ( event )  {
        case IvyAddBind:
                if ( fbindcallback )
					printf("%sApplication: %s on %s add regexp %d : %s\n",
						prefix, IvyGetApplicationName( app ), IvyGetApplicationHost(app), id, regexp);
                break;
        case IvyRemoveBind:
                if ( fbindcallback )
					printf("%sApplication: %s on %s remove regexp %d :%s\n",
						prefix, IvyGetApplicationName( app ), IvyGetApplicationHost(app), id, regexp);
                break;
        case IvyFilterBind:
                printf("%sApplication: %s on %s as been filtred regexp %d :%s\n",
					prefix, IvyGetApplicationName( app ), IvyGetApplicationHost(app), id, regexp);
                break;
        case IvyChangeBind:
                if ( fbindcallback )
					printf("%sApplication: %s on %s change regexp %d : %s\n",
						prefix, IvyGetApplicationName( app ), IvyGetApplicationHost(app), id, regexp);
                break;
        default:
                printf("%sApplication: %s unkown event %d\n", prefix, IvyGetApplicationName( app ), event);
                break;
        }
}


#ifdef IVYMAINLOOP
void TimerCall(TimerId id, void *user_data, unsigned long delta)
{
	char message[64];
	printf("Timer callback: %ld delta %lu ms\n", (long)user_data, delta);
	snprintf(message, sizeof(message), "TEST TIMER %ld", (long) user_data);
	ProbeSendMsgAll(message);
	/*if  ((int)user_data == 5) TimerModify (id, 2000);*/
}
#endif
#ifdef GLUTMAINLLOP
void
display(void)
{
  glClear(GL_COLOR_BUFFER_BIT);
  glFlush();
}
#endif

void BindMsgOfFile( const char * regex_file )
{
	char line[4096];
	size_t size;
	FILE* file;
	file = fopen( regex_file, "r" );
	if ( !file ) {
		perror( "Regexp file open ");
		return;
	}
	while( !feof( file ) )
	{
	if ( fgets( line, sizeof(line), file ) )
		{
		size = strlen(line);
		if ( size > 1 )
			{
			line[size-1] = '\0'; /* supress \n */
			ProbeBindAll(line);
			}
		}
	}
	fclose(file);
}
void BuildFilterRegexp()
{
	char *word=strtok( classes, "," );
	while ( word != NULL && (filter_count < 4096 ))
	{
	filter[filter_count++] = word;
	word = strtok( NULL, ",");
	}
	if ( filter_count )
	IvyBindingSetFilter( filter_count, filter );
}
int main(int argc, char *argv[])
{
	int c;
	int timer_test = 0;
	const char* regex_file = 0;
	char agentnamebuf [1024] = "";
	const char* agentname = DEFAULT_IVYPROBE_NAME;
	char agentready [1024] = "";
	const char* helpmsg =
	  "[options] [regexps]\n\t-b bus\tdefines an Ivy bus to connect to, can be repeated; IVYBUS is also used when set\n"
	  "\t-t\ttriggers the timer test\n"
	  "\t-n name\tchanges the name of the agent, defaults to IVYPROBE\n"
	  "\t-v\tprints the ivy relase number\n\n"
	  "regexp is a Perl5 compatible regular expression (see ivyprobe(1) and pcrepattern(3) for more info\n"
	  "use .help within ivyprobe\n"
	  "\t-s bindcall\tactive the interception of regexp's subscribing or unscribing\n"
	  "\t-f regexfile\tread list of regexp's from file one by line\n"
	  "\t-c msg1,msg2,msg3,...\tfilter the regexp's not beginning with words\n"
	  ;
	while ((c = getopt(argc, argv, "vn:d:b:w:t:sf:c:")) != EOF)
			switch (c) {
			case 'b':
				if (!ProbeAddBus(optarg)) {
					fprintf(stderr, "unable to add bus %s\n", optarg);
					exit(1);
				}
				break;
			case 'w':
				wait_count = atoi(optarg) ;
				break;
			case 'f':
				regex_file = optarg ;
				break;
			case 'n':
				snprintf(agentnamebuf, sizeof(agentnamebuf), "%s", optarg);
				agentname=agentnamebuf;
				break;
			case 'v':
				printf("ivy c library version %d.%d\n",IVYMAJOR_VERSION,IVYMINOR_VERSION);
				break;
			case 't':
			        timer_test = 1;
			        break;
			case 's':
			        fbindcallback=1;
				break;
			case 'c':
			        classes= strdup(optarg);
				break;
			default:
				printf("usage: %s %s",argv[0],helpmsg);
				exit(1);
			}
	{
		static const char ready[] = " Ready";
		snprintf(agentready, sizeof(agentready), "%.*s%s",
			 (int) (sizeof(agentready) - sizeof(ready)), agentname, ready);
	}

	/* Mainloop management */
#ifdef XTMAINLOOP
	/*XtToolkitInitialize();*/
	cntx = XtCreateApplicationContext();
	IvyXtChannelAppContext (cntx);
#endif
#ifdef GLUTMAINLLOOP
	glutInit(&argc, argv);
	glutCreateWindow("IvyProbe Test");
	glClearColor(0.49, 0.62, 0.75, 0.0);
	glutDisplayFunc(display);
#endif
	ProbeAddConfiguredBuses();
	if ( classes )
		BuildFilterRegexp();
	if (!ProbeCreateBuses(agentname, agentready)) {
		ProbeDestroyBuses();
		exit(1);
	}
	if ( regex_file )
		BindMsgOfFile( regex_file );
	for  (; optind < argc; optind++)
	{
		printf("Binding to '%s'\n", argv[optind] );
		ProbeBindAll(argv[optind]);
	}

	if (!ProbeStartBuses()) {
		ProbeStopBuses();
		ProbeJoinBuses();
		ProbeDestroyBuses();
		exit(1);
	}
	ProbeWaitForApplications();

	if  (timer_test) {
#ifdef IVYMAINLOOP
		fprintf(stderr, "ivyprobe: -t timer test is not available with multibus mode yet\n");
#endif
	}

#ifdef WIN32
	printf("Stdin not compatible with select , select only accept socket on Windows\n");
	if (probe_bus_count > 0)
		IvyContextMainLoop(probe_buses[0].ctx);
#else
	{
		char buf[4096];
		while (probe_running && fgets(buf, sizeof(buf), stdin))
			DispatchProbeCommand(buf);
	}
#endif

#ifdef XTMAINLOOP
	XtAppMainLoop (cntx);
#endif
#ifdef GLIBMAINLOOP
	{
	  GMainLoop *ml =  g_main_loop_new(NULL, FALSE);
	  g_main_loop_run(ml);
	}
#endif
#ifdef GLUTMAINLOOP
	glutMainLoop();
#endif

	ProbeStopBuses();
	ProbeJoinBuses();
	ProbeDestroyBuses();
	return 0;
}
