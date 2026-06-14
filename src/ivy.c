/*
 *
 *	Ivy, C interface
 *
 *	Copyright 1997-2024
 *	Centre d'Etudes de la Navigation Aerienne
 *
 *	Main functions
 *
 *	Authors: Francois-Regis Colin,Stephane Chatty, Alexandre Bustico, Mathieu Poirier
 *
 *	$Id: ivy.c 3602 2014-04-07 08:35:53Z bustico $
 *
 *	Please refer to file version.h for the
 *	copyright notice regarding this software
 */

/*
  TODO :  ° faire un configure
*/

#ifdef OPENMP
#include <omp.h>
#endif

#include <stdlib.h>
#include <stdint.h>
#ifdef __MINGW32__
#include <sys/time.h>
#include <Ws2tcpip.h>
#include <windows.h>
#include "timer.h"
#define snprintf _snprintf
#if 0 //def __MINGW32__
// should be removed in when defined in MinGW include of ws2tcpip.h
extern const char * WSAAPI inet_ntop(int af, const void *src,
                             char *dst, socklen_t size);
extern int WSAAPI inet_pton(int af, const char *src, void *dst);

#endif
#else
#include <sys/time.h>
#include <arpa/inet.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include <fcntl.h>


#include "version.h"
#include "param.h"
#include "uthash.h"
#include "intervalRegexp.h"
#include "ivychannel.h"
#include "ivyloop.h"
#include "ivysocket.h"
#include "list.h"
#include "ivybuffer.h"
#include "ivydebug.h"
#include "ivybind.h"
#include "ivythread.h"
#include "ivy.h"

#define ARG_START "\002"
#define ARG_END "\003"

#if defined(_MSC_VER)
#define IVY_TLS __declspec(thread)
#elif defined(__GNUC__)
#define IVY_TLS __thread
#else
#define IVY_TLS _Thread_local
#endif

#ifdef __APPLE__
#define DEFAULT_DOMAIN 127.0.0.1
#else
#define DEFAULT_DOMAIN 127.255.255.255
#endif

#ifdef __MINGW32__
#ifndef timersub
#define timersub(a, b, result)\
	do {												\
		(result)->tv_sec = (a)->tv_sec - (b)->tv_sec;	\
		(result)->tv_usec = (a)->tv_usec - (b)->tv_usec;\
	if ((result)->tv_usec < 0) {						\
		--(result)->tv_sec;								\
	  (result)->tv_usec += 1000000;						\
	}													\
	} while (0)
#endif
#endif

/* stringification et concatenation du domaine et du port en 2 temps :
 * Obligatoire puisque la substitution de domain, et de bus n'est pas
 * effectuée si on stringifie directement dans la macro GenerateIvyBus */
#define str(bus) #bus
#define GenerateIvyBus(domain,bus) str(domain)":"str(bus)
#define       MIN(a, b)   ((a) > (b) ? (b) : (a))

#define DefaultIvyBus GenerateIvyBus(DEFAULT_DOMAIN,IVY_DEFAULT_BUS)
#define MAX_APPLICATION_MESSAGES 4096

typedef enum {
	Bye,			/* l'application emettrice se termine */
	AddRegexp,		/* expression reguliere d'un client */
	Msg,			/* message reel */
	Error,			/* error message */
	DelRegexp,		/* Remove expression reguliere */
	EndRegexp,		/* end of the regexp list */
	StartRegexp,		/* debut des expressions */
	DirectMsg,		/* message direct a destination de l'appli */
	Die,			/* demande de terminaison de l'appli */
	Ping,			/* message de controle ivy */
	Pong			/* ivy doit renvoyer ce message à la reception d'un ping */
} MsgType;


typedef struct _msg_snd_dict	*MsgSndDictPtr;
typedef struct _global_reg_lst	*GlobRegPtr;


struct _msg_rcv {			/* requete d'emission d'un client */
	MsgRcvPtr next;
	int id;
	char *regexp;		/* regexp du message a recevoir */
	MsgCallback callback;		/* callback a declancher a la reception */
	void *user_data;		/* stokage d'info client */
};



/* liste de regexps source */
struct _global_reg_lst {		/* liste des regexp source */
	GlobRegPtr next;
	char *str_regexp;		/* la regexp sous forme source */
  	int id;                         /* son id, differente pour chaque client */
};


/* pour le dictionnaire clef=regexp, valeur = cette struct */
struct _msg_snd_dict {			/* requete de reception d'un client */
        UT_hash_handle hh;		/* makes this structure hashable */
        char *regexp_src;		/* clef du dictionnaire (hash uthash) */
        RWIvyClientPtr clientList;        /* liste des clients */
	IvyBinding binding;		/* la regexp sous forme machine */
};

/* liste de clients, champ de la struct _msg_snd_dict qui est valeur du dictionnaire */
/* typedef IvyClientPtr */

struct _ping_timestamp {
  struct timeval ts;
  int		 id;
};

struct _clnt_lst_dict {
	RWIvyClientPtr next;
	Client client;			/* la socket  client */

	char *app_name;			/* nom de l'application */
	unsigned short app_port;	/* port de l'application */
	int id;                         /* l'id n'est pas liée uniquement
 					   a la regexp, mais au couple
	 				   regexp, client */
        GlobRegPtr srcRegList;          /* liste de regexp source */
#ifdef OPENMP
       int endRegexpReceived;
#endif // OPENMP
       int readyToSend;		        /* comptage des endRegexps recu et emis */
       int ignore_subsequent_msg;	/* pour ignorer les messages venant
					   d'une socket ferme, mais donc les donnees sont deja en buffer */
       struct  _ping_timestamp ping_timestamp;   /* on enregistre le timestamp du ping pour envoyer le roundtrip à
					   la reception du pong */
};

struct IvyContext {
  IvyMutex ivy_mutex;
  IvyCond ivy_stop_done;
  IvyCond ivy_callbacks_done;
  int ivy_callbacks_active;
  IvyRwLock ivy_bindings_rwlock;
  IvyThreadId ivy_owner_thread;
  int ivy_owner_thread_set;
  IvyContextState ivy_state;
  IvyChannelState *ivy_loop;
  SocketState *ivy_sockets;

  /* flag pour le debug en cas de Filter de regexp */
  int ivy_debug_filter;
  /* flag pour le debug en cas de message binaire */
  int ivy_debug_binary_msg;
  /* mode IPV6 pour les sockets */
  int ivy_ipv6;
  /* server  pour la socket application */
  Server ivy_server;
  /* numero de port TCP en mode serveur */
  unsigned short ivy_application_port;
  /* numero de port UDP */
  unsigned short ivy_supervision_port;
  /* client pour la socket supervision */
  Client ivy_broadcast;
  char *ivy_application_name;
  const char *ivy_application_id;
  char ivy_application_id_buffer[128];

  /* callback appele sur reception d'un message direct */
  MsgDirectCallback ivy_direct_callback;
  void *ivy_direct_user_data;

  /* callback appele sur changement d'etat d'application */
  IvyApplicationCallback ivy_application_callback;
  void *ivy_application_user_data;

  /* callback appele sur ajout suppression de regexp */
  IvyBindCallback ivy_application_bind_callback;
  void *ivy_application_bind_data;

  /* callback appele sur demande de terminaison d'application */
  IvyDieCallback ivy_application_die_callback;
  void *ivy_application_die_user_data;

  /* callback appele sur reception d'une trame PONG */
  IvyPongCallback ivy_application_pong_callback;

  /* liste des messages a recevoir */
  MsgRcvPtr ivy_msg_recv;

  /* liste des clients connectes */
  RWIvyClientPtr ivy_all_clients;

  /* dictionnaire clef : regexp, valeur : liste de clients IvyClientPtr */
  MsgSndDictPtr ivy_mess_snd_by_regexp;

  char *ivy_ready_message;
  int ivy_recv_id;
  IvyBuffer ivy_bind_buffer;
  IvyBuffer ivy_change_buffer;
  IvyBuffer ivy_send_buffer;
  IvyBuffer ivy_send_error_buffer;
  IvyBuffer ivy_regexp_call_buffer;
  IvyBuffer ivy_regexp_call_unique_buffer;
  char ivy_application_list[4096];
  char *ivy_application_messages[MAX_APPLICATION_MESSAGES + 1];
  char ivy_regexp_error_buffer[1024];
  char *ivy_next_arg_start;
  char *ivy_next_arg_end;
#ifdef OPENMP
  struct {
    MsgSndDictPtr *msgPtrArray;
    int size;
    int numPtr;
  } ivy_omp_dict_cache;
#endif
};

static IvyContext *default_ctx = NULL;
static IVY_TLS IvyStatus ivy_last_error = IVY_OK;
static IVY_TLS int ivy_callback_depth = 0;
static IVY_TLS IvyContext *ivy_current_context = NULL;

static IvyContext *IvyGetDefaultContext(void);
static IvyContext *IvyGetCurrentContext(void);
static void IvySetCurrentContext(IvyContext *ctx);
static IvyContext *IvyPushCurrentContext(IvyContext *ctx);
static void IvyPopCurrentContext(IvyContext *previous);
static IvyStatus IvySetLastError(IvyStatus status);
static int IvyReturnStatus(IvyStatus status);
static int IvyReturnQueryBufferSize(size_t content_size, size_t buffer_size);
static int IvyContextRejectIfStopped(const IvyContext *ctx);
static void IvyContextSetState(IvyContext *ctx, IvyContextState state);
static int IvyContextStateIs(const IvyContext *ctx, IvyContextState state);
static void IvyContextStopInLoop(void *data);
static void IvyBindingsReadLock(IvyContext *ctx);
static void IvyBindingsReadUnlock(IvyContext *ctx);
static void IvyBindingsWriteLock(IvyContext *ctx);
static void IvyBindingsWriteUnlock(IvyContext *ctx);
static void IvyDispatchApplicationCallback(IvyContext *ctx, IvyClientPtr app, IvyApplicationEvent event);
static void IvyDispatchBindCallback(IvyContext *ctx, IvyClientPtr app, int id, const char *regexp, IvyBindEvent event);
static void IvyDispatchDieCallback(IvyContext *ctx, IvyClientPtr app, int id);
static void IvyDispatchPongCallback(IvyContext *ctx, IvyClientPtr app, int roundTripOrTimout);
static void IvyDispatchDirectCallback(IvyContext *ctx, IvyClientPtr app, int id, char *msg);
static void substituteInterval (IvyBuffer *src);
static int ParseIvyIPv4Broadcast(const char *start, const char *end, uint32_t *out);

typedef struct _ivy_app_callback_event {
  struct _ivy_app_callback_event *next;
  IvyClientPtr app;
  IvyApplicationEvent event;
} IvyAppCallbackEvent;

typedef struct {
  IvyAppCallbackEvent *head;
  IvyAppCallbackEvent *tail;
  IvyMutex mutex;
  int mutex_initialized;
} IvyAppCallbackQueue;

static void IvyAppCallbackQueueInit(IvyAppCallbackQueue *queue);
static void IvyAppCallbackQueueDestroy(IvyAppCallbackQueue *queue);
static void IvyAppCallbackQueueAdd(IvyAppCallbackQueue *queue, IvyClientPtr app, IvyApplicationEvent event);
static void IvyAppCallbackQueueAddSendState(IvyAppCallbackQueue *queue, IvyClientPtr app, SendState state);
static void IvyAppCallbackQueueDispatch(IvyContext *ctx, IvyAppCallbackQueue *queue);
static void IvyDispatchSendStateCallback(IvyContext *ctx, IvyClientPtr app, SendState state);

static int RegexpCall (const MsgSndDictPtr msg, const char * const message, IvyAppCallbackQueue *callbacks);
static int RegexpCallUnique (const MsgSndDictPtr msg, const char * const message,
			     const Client clientUnique, IvyAppCallbackQueue *callbacks);
static IvyStatus IvyStatusFromSendState(SendState state);
static MsgRcvPtr IvyContextBindMsgV(IvyContext *ctx,
	MsgCallback callback, void *user_data, const char *fmt_regex, va_list ap);
static MsgRcvPtr IvyContextChangeMsgV(IvyContext *ctx,
	MsgRcvPtr msg, const char *fmt_regex, va_list ap);
static int IvyContextSendMsgV(IvyContext *ctx, const char *fmt, va_list ap);
static int IvyContextSendErrorV(IvyContext *ctx,
	IvyClientPtr app, int id, const char *fmt, va_list ap);

static void freeClient ( RWIvyClientPtr client);
static void delOneClient (const Client client);

static void delRegexpForOneClientFromDictionary (const char *regexp, IvyClientPtr client);
static void delOneIvyClientFromDictionaryEntry (MsgSndDictPtr msgSendDict,
						IvyClientPtr client);
static void delOneClientFromDictionaryEntry (MsgSndDictPtr msgSendDict,
					     const Client client);
static void delAllRegexpsFromDictionary ();
static void addRegexpToDictionary (const char* regexp, IvyClientPtr client);
static void changeRegexpInDictionary (const char* old_regexp, const char* new_regexp, IvyClientPtr client);

static char delRegexpForOneClient (IvyClientPtr client, int id);
static IvyBindEvent addRegexp (const char* regexp, IvyClientPtr client);
static IvyBindEvent changeRegexp (const char* old_regexp, const char* new_regexp, IvyClientPtr client);
static IvyBindEvent addOrChangeRegexp (const char* regexp, IvyClientPtr client);
static int IvyCheckBuffer( const char* buffer );

#ifdef OPENMP
static void regenerateRegPtrArrayCache ();
static void addRegToPtrArrayCache (MsgSndDictPtr newReg);
#endif

#define debug_filter (IvyGetCurrentContext()->ivy_debug_filter)
#define debug_binary_msg (IvyGetCurrentContext()->ivy_debug_binary_msg)
#define ipv6 (IvyGetCurrentContext()->ivy_ipv6)
#define server (IvyGetCurrentContext()->ivy_server)
#define ApplicationPort (IvyGetCurrentContext()->ivy_application_port)
#define SupervisionPort (IvyGetCurrentContext()->ivy_supervision_port)
#define broadcast (IvyGetCurrentContext()->ivy_broadcast)
#define ApplicationName (IvyGetCurrentContext()->ivy_application_name)
#define ApplicationID (IvyGetCurrentContext()->ivy_application_id)
#define msg_recv (IvyGetCurrentContext()->ivy_msg_recv)
#define allClients (IvyGetCurrentContext()->ivy_all_clients)
#define messSndByRegexp (IvyGetCurrentContext()->ivy_mess_snd_by_regexp)
#define ready_message (IvyGetCurrentContext()->ivy_ready_message)
#ifdef OPENMP
#define ompDictCache (IvyGetCurrentContext()->ivy_omp_dict_cache)
#endif

#define MAXPORT(a,b)      ((a>b) ? a : b)

static IvyStatus IvySetLastError(IvyStatus status)
{
	ivy_last_error = status;
	return status;
}

static int IvyReturnStatus(IvyStatus status)
{
	IvySetLastError(status);
	return (int)status;
}

static int IvyReturnQueryBufferSize(size_t content_size, size_t buffer_size)
{
	size_t required_size;

	if (content_size > (size_t)INT_MAX - 1)
		return IvyReturnStatus(IVY_ENOMEM);
	required_size = content_size + 1;
	if (buffer_size > 0 && required_size > buffer_size)
		IvySetLastError(IVY_ENOMEM);
	else
		IvySetLastError(IVY_OK);
	return (int)required_size;
}

IvyStatus IvyGetLastError(void)
{
	return ivy_last_error;
}

IvyContextState IvyContextGetState(const IvyContext *ctx)
{
	IvyContextState state;

	if (!ctx) {
		IvySetLastError(IVY_EINVAL);
		return IVY_CTX_DESTROYED;
	}
	IvyMutexLock((IvyMutex *)&ctx->ivy_mutex);
	state = ctx->ivy_state;
	IvyMutexUnlock((IvyMutex *)&ctx->ivy_mutex);
	IvySetLastError(IVY_OK);
	return state;
}

static int IvyContextRejectIfStopped(const IvyContext *ctx)
{
	IvyContextState state;

	if (!ctx)
		return IvyReturnStatus(IVY_EINVAL);
	IvyMutexLock((IvyMutex *)&ctx->ivy_mutex);
	state = ctx->ivy_state;
	IvyMutexUnlock((IvyMutex *)&ctx->ivy_mutex);
	if (state == IVY_CTX_STOPPING || state == IVY_CTX_STOPPED)
		return IvyReturnStatus(IVY_ESTOPPED);
	return IvyReturnStatus(IVY_OK);
}

static void IvyContextSetState(IvyContext *ctx, IvyContextState state)
{
	IvyMutexLock(&ctx->ivy_mutex);
	ctx->ivy_state = state;
	if (state == IVY_CTX_STOPPED || state == IVY_CTX_DESTROYED)
		IvyCondBroadcast(&ctx->ivy_stop_done);
	IvyMutexUnlock(&ctx->ivy_mutex);
}

static int IvyContextStateIs(const IvyContext *ctx, IvyContextState state)
{
	int is_state;

	IvyMutexLock((IvyMutex *)&ctx->ivy_mutex);
	is_state = ctx->ivy_state == state;
	IvyMutexUnlock((IvyMutex *)&ctx->ivy_mutex);
	return is_state;
}

static void IvyBindingsReadLock(IvyContext *ctx)
{
	IvyRwLockRdLock(&ctx->ivy_bindings_rwlock);
}

static void IvyBindingsReadUnlock(IvyContext *ctx)
{
	IvyRwLockUnlockRead(&ctx->ivy_bindings_rwlock);
}

static void IvyBindingsWriteLock(IvyContext *ctx)
{
	IvyRwLockWrLock(&ctx->ivy_bindings_rwlock);
}

static void IvyBindingsWriteUnlock(IvyContext *ctx)
{
	IvyRwLockUnlockWrite(&ctx->ivy_bindings_rwlock);
}

static void IvyCallbackEnter(IvyContext *ctx)
{
	IvyMutexLock(&ctx->ivy_mutex);
	ctx->ivy_callbacks_active++;
	IvyMutexUnlock(&ctx->ivy_mutex);
	ivy_callback_depth++;
}

static void IvyCallbackLeave(IvyContext *ctx)
{
	if (ivy_callback_depth > 0)
		ivy_callback_depth--;
	IvyMutexLock(&ctx->ivy_mutex);
	if (ctx->ivy_callbacks_active > 0)
		ctx->ivy_callbacks_active--;
	if (ctx->ivy_callbacks_active == 0)
		IvyCondBroadcast(&ctx->ivy_callbacks_done);
	IvyMutexUnlock(&ctx->ivy_mutex);
}

static int IvyContextWaitForCallbacks(IvyContext *ctx)
{
	if (ivy_callback_depth > 0)
		return IvyReturnStatus(IVY_ESTATE);

	IvyMutexLock(&ctx->ivy_mutex);
	while (ctx->ivy_callbacks_active > 0)
		IvyCondWait(&ctx->ivy_callbacks_done, &ctx->ivy_mutex);
	IvyMutexUnlock(&ctx->ivy_mutex);
	return IvyReturnStatus(IVY_OK);
}

static void IvyDispatchApplicationCallback(IvyContext *ctx, IvyClientPtr app, IvyApplicationEvent event)
{
	IvyApplicationCallback callback;
	void *user_data;

	IvyMutexLock(&ctx->ivy_mutex);
	callback = ctx->ivy_application_callback;
	user_data = ctx->ivy_application_user_data;
	IvyMutexUnlock(&ctx->ivy_mutex);

	if (!callback)
		return;

	IvyCallbackEnter(ctx);
	(*callback)(app, user_data, event);
	IvyCallbackLeave(ctx);
}

static void IvyDispatchBindCallback(IvyContext *ctx, IvyClientPtr app, int id, const char *regexp, IvyBindEvent event)
{
	IvyBindCallback callback;
	void *user_data;

	IvyMutexLock(&ctx->ivy_mutex);
	callback = ctx->ivy_application_bind_callback;
	user_data = ctx->ivy_application_bind_data;
	IvyMutexUnlock(&ctx->ivy_mutex);

	if (!callback)
		return;

	IvyCallbackEnter(ctx);
	(*callback)(app, user_data, id, regexp, event);
	IvyCallbackLeave(ctx);
}

static void IvyDispatchDieCallback(IvyContext *ctx, IvyClientPtr app, int id)
{
	IvyDieCallback callback;
	void *user_data;

	IvyMutexLock(&ctx->ivy_mutex);
	callback = ctx->ivy_application_die_callback;
	user_data = ctx->ivy_application_die_user_data;
	IvyMutexUnlock(&ctx->ivy_mutex);

	if (!callback)
		return;

	IvyCallbackEnter(ctx);
	(*callback)(app, user_data, id);
	IvyCallbackLeave(ctx);
}

static void IvyDispatchPongCallback(IvyContext *ctx, IvyClientPtr app, int roundTripOrTimout)
{
	IvyPongCallback callback;

	IvyMutexLock(&ctx->ivy_mutex);
	callback = ctx->ivy_application_pong_callback;
	IvyMutexUnlock(&ctx->ivy_mutex);

	if (!callback)
		return;

	IvyCallbackEnter(ctx);
	(*callback)(app, roundTripOrTimout);
	IvyCallbackLeave(ctx);
}

static void IvyDispatchDirectCallback(IvyContext *ctx, IvyClientPtr app, int id, char *msg)
{
	MsgDirectCallback callback;
	void *user_data;

	IvyMutexLock(&ctx->ivy_mutex);
	callback = ctx->ivy_direct_callback;
	user_data = ctx->ivy_direct_user_data;
	IvyMutexUnlock(&ctx->ivy_mutex);

	if (!callback)
		return;

	IvyCallbackEnter(ctx);
	(*callback)(app, user_data, id, msg);
	IvyCallbackLeave(ctx);
}

static int IvyHasPongCallback(IvyContext *ctx)
{
	int has_callback;

	IvyMutexLock(&ctx->ivy_mutex);
	has_callback = ctx->ivy_application_pong_callback != NULL;
	IvyMutexUnlock(&ctx->ivy_mutex);
	return has_callback;
}

static void IvyAppCallbackQueueInit(IvyAppCallbackQueue *queue)
{
	queue->head = NULL;
	queue->tail = NULL;
	queue->mutex_initialized = IvyMutexInit(&queue->mutex) == 0;
}

static void IvyAppCallbackQueueDestroy(IvyAppCallbackQueue *queue)
{
	IvyAppCallbackEvent *event;

	while (queue->head) {
		event = queue->head;
		queue->head = event->next;
		free(event);
	}
	queue->tail = NULL;
	if (queue->mutex_initialized) {
		IvyMutexDestroy(&queue->mutex);
		queue->mutex_initialized = 0;
	}
}

static void IvyAppCallbackQueueAdd(IvyAppCallbackQueue *queue, IvyClientPtr app, IvyApplicationEvent event_type)
{
	IvyAppCallbackEvent *event;

	event = (IvyAppCallbackEvent *)malloc(sizeof(*event));
	if (!event)
		return;
	event->next = NULL;
	event->app = app;
	event->event = event_type;

	if (queue->mutex_initialized)
		IvyMutexLock(&queue->mutex);
	if (queue->tail)
		queue->tail->next = event;
	else
		queue->head = event;
	queue->tail = event;
	if (queue->mutex_initialized)
		IvyMutexUnlock(&queue->mutex);
}

static void IvyAppCallbackQueueAddSendState(IvyAppCallbackQueue *queue, IvyClientPtr app, SendState state)
{
	switch (state) {
	case SendStateChangeToCongestion:
		IvyAppCallbackQueueAdd(queue, app, IvyApplicationCongestion);
		break;
	case SendStateFifoFull:
		IvyAppCallbackQueueAdd(queue, app, IvyApplicationFifoFull);
		break;
	default:
		break;
	}
}

static void IvyAppCallbackQueueDispatch(IvyContext *ctx, IvyAppCallbackQueue *queue)
{
	for (;;) {
		IvyAppCallbackEvent *event;

		if (queue->mutex_initialized)
			IvyMutexLock(&queue->mutex);
		event = queue->head;
		if (event) {
			queue->head = event->next;
			if (!queue->head)
				queue->tail = NULL;
		}
		if (queue->mutex_initialized)
			IvyMutexUnlock(&queue->mutex);

		if (!event)
			break;

		IvyDispatchApplicationCallback(ctx, event->app, event->event);
		free(event);
	}
}

static void IvyDispatchSendStateCallback(IvyContext *ctx, IvyClientPtr app, SendState state)
{
	switch (state) {
	case SendStateChangeToCongestion:
		IvyDispatchApplicationCallback(ctx, app, IvyApplicationCongestion);
		break;
	case SendStateFifoFull:
		IvyDispatchApplicationCallback(ctx, app, IvyApplicationFifoFull);
		break;
	default:
		break;
	}
}

IvyContext *IvyContextCreate(
	 const char *appname,
	 const char *ready,
	 IvyApplicationCallback callback,
	 void *data,
	 IvyDieCallback die_callback,
	 void *die_data
	 )
{
	IvyContext *ctx = (IvyContext *) calloc(1, sizeof(*ctx));
	if (!ctx) {
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}

	if (IvyMutexInit(&ctx->ivy_mutex) != 0) {
		free(ctx);
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}
	if (IvyCondInit(&ctx->ivy_stop_done) != 0) {
		IvyMutexDestroy(&ctx->ivy_mutex);
		free(ctx);
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}
	if (IvyCondInit(&ctx->ivy_callbacks_done) != 0) {
		IvyCondDestroy(&ctx->ivy_stop_done);
		IvyMutexDestroy(&ctx->ivy_mutex);
		free(ctx);
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}
	if (IvyRwLockInit(&ctx->ivy_bindings_rwlock) != 0) {
		IvyCondDestroy(&ctx->ivy_callbacks_done);
		IvyCondDestroy(&ctx->ivy_stop_done);
		IvyMutexDestroy(&ctx->ivy_mutex);
		free(ctx);
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}
	ctx->ivy_loop = IvyChannelStateCreate();
	if (!ctx->ivy_loop) {
		IvyRwLockDestroy(&ctx->ivy_bindings_rwlock);
		IvyCondDestroy(&ctx->ivy_callbacks_done);
		IvyCondDestroy(&ctx->ivy_stop_done);
		IvyMutexDestroy(&ctx->ivy_mutex);
		free(ctx);
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}
	ctx->ivy_sockets = SocketStateCreate(ctx->ivy_loop, ctx);
	if (!ctx->ivy_sockets) {
		IvyChannelStateDestroy(ctx->ivy_loop);
		IvyRwLockDestroy(&ctx->ivy_bindings_rwlock);
		IvyCondDestroy(&ctx->ivy_callbacks_done);
		IvyCondDestroy(&ctx->ivy_stop_done);
		IvyMutexDestroy(&ctx->ivy_mutex);
		free(ctx);
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}

	ctx->ivy_state = IVY_CTX_CREATED;
	if (appname) {
		ctx->ivy_application_name = strdup(appname);
		if (!ctx->ivy_application_name) {
			SocketStateDestroy(ctx->ivy_sockets);
			IvyChannelStateDestroy(ctx->ivy_loop);
			IvyRwLockDestroy(&ctx->ivy_bindings_rwlock);
			IvyCondDestroy(&ctx->ivy_callbacks_done);
			IvyCondDestroy(&ctx->ivy_stop_done);
			IvyMutexDestroy(&ctx->ivy_mutex);
			free(ctx);
			IvySetLastError(IVY_ENOMEM);
			return NULL;
		}
	}
	ctx->ivy_application_callback = callback;
	ctx->ivy_application_user_data = data;
	ctx->ivy_application_die_callback = die_callback;
	ctx->ivy_application_die_user_data = die_data;
	if (ready) {
		ctx->ivy_ready_message = strdup(ready);
		if (!ctx->ivy_ready_message) {
			free(ctx->ivy_application_name);
			SocketStateDestroy(ctx->ivy_sockets);
			IvyChannelStateDestroy(ctx->ivy_loop);
			IvyRwLockDestroy(&ctx->ivy_bindings_rwlock);
			IvyCondDestroy(&ctx->ivy_callbacks_done);
			IvyCondDestroy(&ctx->ivy_stop_done);
			IvyMutexDestroy(&ctx->ivy_mutex);
			free(ctx);
			IvySetLastError(IVY_ENOMEM);
			return NULL;
		}
	}
	if (getenv("IVY_DEBUG_BINARY"))
		ctx->ivy_debug_binary_msg = 1;
	IvySetLastError(IVY_OK);
	return ctx;
}

int IvyContextDestroy(IvyContext *ctx)
{
	MsgRcvPtr msg;
	MsgRcvPtr next_msg;

	if (!ctx)
		return IvyReturnStatus(IVY_EINVAL);

	if (IvyContextStateIs(ctx, IVY_CTX_RUNNING) || IvyContextStateIs(ctx, IVY_CTX_STARTING)) {
		int status = IvyContextStop(ctx);
		if (status != IVY_OK)
			return status;
	}
	{
		int status = IvyContextWaitForCallbacks(ctx);
		if (status != IVY_OK)
			return status;
	}

	if (ctx == default_ctx)
		default_ctx = NULL;

	free(ctx->ivy_application_name);
	free(ctx->ivy_ready_message);
	free(ctx->ivy_bind_buffer.data);
	free(ctx->ivy_change_buffer.data);
	free(ctx->ivy_send_buffer.data);
	free(ctx->ivy_send_error_buffer.data);
	free(ctx->ivy_regexp_call_buffer.data);
	free(ctx->ivy_regexp_call_unique_buffer.data);
#ifdef OPENMP
	free(ctx->ivy_omp_dict_cache.msgPtrArray);
#endif

	for (msg = ctx->ivy_msg_recv; msg; msg = next_msg) {
		next_msg = msg->next;
		free(msg->regexp);
		free(msg);
	}
	ctx->ivy_msg_recv = NULL;

	IvyChannelStateDestroy(ctx->ivy_loop);
	ctx->ivy_loop = NULL;
	SocketStateDestroy(ctx->ivy_sockets);
	ctx->ivy_sockets = NULL;

	IvyContextSetState(ctx, IVY_CTX_DESTROYED);
	IvyRwLockDestroy(&ctx->ivy_bindings_rwlock);
	IvyCondDestroy(&ctx->ivy_callbacks_done);
	IvyCondDestroy(&ctx->ivy_stop_done);
	IvyMutexDestroy(&ctx->ivy_mutex);
	free(ctx);
	return IvyReturnStatus(IVY_OK);
}

static IvyContext *IvyGetDefaultContext(void)
{
	if (!default_ctx) {
		default_ctx = IvyContextCreate(NULL, NULL, NULL, NULL, NULL, NULL);
		if (!default_ctx) {
			perror("Ivy default context allocation");
			exit(-1);
		}
		SocketStateDestroy(default_ctx->ivy_sockets);
		IvyChannelStateDestroy(default_ctx->ivy_loop);
		default_ctx->ivy_loop = IvyChannelGetDefaultState();
		default_ctx->ivy_sockets = SocketGetDefaultState();
	}
	return default_ctx;
}

static IvyContext *IvyGetCurrentContext(void)
{
	return ivy_current_context ? ivy_current_context : IvyGetDefaultContext();
}

static void IvySetCurrentContext(IvyContext *ctx)
{
	ivy_current_context = ctx;
}

static IvyContext *IvyPushCurrentContext(IvyContext *ctx)
{
	IvyContext *previous = ivy_current_context;
	ivy_current_context = ctx ? ctx : IvyGetDefaultContext();
	return previous;
}

static void IvyPopCurrentContext(IvyContext *previous)
{
	ivy_current_context = previous;
}

static IvyContext *IvyContextFromSocketClient(Client client)
{
	IvyContext *ctx = (IvyContext *)SocketGetOwnerData(client);
	return ctx ? ctx : IvyGetDefaultContext();
}

#ifdef IVY_TESTING
int IvyLegacyDefaultContextIsInitialized(void)
{
	return default_ctx != NULL;
}

int IvyTestingSetDefaultContextState(IvyContextState state)
{
	IvyContextSetState(IvyGetDefaultContext(), state);
	return IVY_OK;
}

IvyContextState IvyTestingGetDefaultContextState(void)
{
	return IvyContextGetState(IvyGetDefaultContext());
}

int IvyTestingContextLoopIsActive(IvyContext *ctx)
{
	if (!ctx)
		return 0;
	return IvyChannelLoopIsActiveFor(ctx->ivy_loop);
}
#endif

/*
 * function like strok but do not eat consecutive separator
 * */
static char * nextArg( char *s, const char *separator )
{
	IvyContext *ctx = IvyGetCurrentContext();
	if ( s )
	{
		ctx->ivy_next_arg_end = s;
	}
	ctx->ivy_next_arg_start = ctx->ivy_next_arg_end;

	while ( *ctx->ivy_next_arg_end && *ctx->ivy_next_arg_end != *separator )
		ctx->ivy_next_arg_end++;
	if ( *ctx->ivy_next_arg_end == *separator ) *ctx->ivy_next_arg_end++ = '\0';
	if ( ctx->ivy_next_arg_end == ctx->ivy_next_arg_start ) return NULL;
	return ctx->ivy_next_arg_start;
}

static SendState MsgSendTo(IvyClientPtr ivyClient,
			   MsgType msgtype, int id, const char *message )
{
  SendState state;

  if (!ivyClient)
    return SendParamError;

  state = SocketSend( ivyClient->client, "%d %d" ARG_START "%s\n", msgtype, id, message);

  return (state);
}

static IvyStatus IvyStatusFromSendState(SendState state)
{
  switch (state) {
  case SendError:
    return IVY_EIO;
  case SendParamError:
    return IVY_EINVAL;
  default:
    return IVY_OK;
  }
}

static void IvyCleanup()
{
	IvyContext *ctx = IvyGetCurrentContext();
	RWIvyClientPtr clnt,next;
	GlobRegPtr   regLst;

	IvyBindingsWriteLock(ctx);

	/* destruction des connexions clients */
	IVY_LIST_EACH_SAFE( allClients, clnt, next )
	{
		/* on dit au revoir */
	  MsgSendTo( clnt, Bye, 0, "" );
		SocketClose( clnt->client );
		IVY_LIST_EACH (clnt->srcRegList, regLst) {
		  if (regLst->str_regexp != NULL) {
		    free (regLst->str_regexp);
		    regLst->str_regexp = NULL;
		  }
		}
		IVY_LIST_EMPTY( clnt->srcRegList );
		IVY_LIST_REMOVE (allClients, clnt);
	}
	IVY_LIST_EMPTY( allClients );
	delAllRegexpsFromDictionary ();

	/* destruction des sockets serveur et supervision */
	SocketServerClose( server );
	SocketClose( broadcast );
	IvyBindingsWriteUnlock(ctx);
}


static int
ClientCall (IvyClientPtr clnt, const char *message)
{
  IvyContext *ctx = IvyGetCurrentContext();
  int match_count = 0;
  IvyAppCallbackQueue callback_events;

  /*   pour toutes les regexp */
  MsgSndDictPtr msgSendDict;

  IvyAppCallbackQueueInit(&callback_events);
  IvyBindingsReadLock(ctx);
  for (msgSendDict=messSndByRegexp; msgSendDict != NULL;
       msgSendDict= (MsgSndDictPtr) msgSendDict->hh.next) {
    match_count += RegexpCallUnique (msgSendDict, message, clnt->client, &callback_events);
  }
  IvyBindingsReadUnlock(ctx);
  IvyAppCallbackQueueDispatch(ctx, &callback_events);
  IvyAppCallbackQueueDestroy(&callback_events);

  TRACE_IF( match_count == 0, "Warning no recipient for %s\n",message);
  /* si le message n'est pas emit et qu'il y a des filtres alors WARNING */
  if ( match_count == 0 && debug_filter )  {
    IvyBindindFilterCheck( message );
  }
  return match_count;
}



static int
RegexpCall (const MsgSndDictPtr msg, const char * const message, IvyAppCallbackQueue *callbacks)
{
  IvyBuffer bufferArg = { NULL, 0, 0 };
  char   bufferId[16];
  int match_count ;
  SendState state;
  int indx;
  int arglen;
  const char *arg;
  IvyClientPtr clnt;
  int rc;

  match_count = 0;
  rc= IvyBindingExec(msg->binding, message );

  if (rc<1) return 0; /* no match */

  bufferArg.offset = 0;
  //  bufferArg.size = bufferId.size = 0;
  //  bufferArg.data = bufferId.data = NULL;

  /* il faut essayer d'envoyer le message en une seule fois sur la socket */
  /* pour eviter au maximun de passer dans le select plusieur fois par message du protocole Ivy */
  /* pour eviter la latence ( PB de perfo detecte par ivyperf ping roudtrip ) */

  TRACE( "Send matching args count %d\n",rc);

  for(  indx=1; indx < rc ; indx++ )
    {
      IvyBindingMatch (msg->binding, message, indx, &arglen, & arg );
      if (make_message_var( &bufferArg,  "%.*s" ARG_END , arglen, arg ) < 0) {
	free(bufferArg.data);
	return match_count;
      }
    }
  if (make_message_var( &bufferArg, "\n") < 0) {
    free(bufferArg.data);
    return match_count;
  }

  IVY_LIST_EACH(msg->clientList, clnt ) {

    snprintf (bufferId, sizeof(bufferId), "%d %d" ARG_START ,Msg, clnt->id);
    state = SocketSendRawWithId(clnt->client, bufferId, bufferArg.data , bufferArg.offset);
    match_count++;
    IvyAppCallbackQueueAddSendState(callbacks, clnt, state);
  }
  free(bufferArg.data);
  return match_count;
}

static int
RegexpCallUnique (const MsgSndDictPtr msg, const char * const message, const
		  Client clientUnique, IvyAppCallbackQueue *callbacks)
{
  IvyBuffer bufferArg = { NULL, 0, 0 };
  char   bufferId[16];
  int match_count ;
  SendState state;
  int indx;
  int arglen;
  const char *arg;
  IvyClientPtr clnt;
  int rc;

  match_count = 0;
  rc= IvyBindingExec(msg->binding, message );

  if (rc<1) return 0; /* no match */

  bufferArg.offset = 0;
  //  bufferArg.size = bufferId.
  //  bufferArg.size = bufferId.size = 0;
  //  bufferArg.data = bufferId.data = NULL;

  /* il faut essayer d'envoyer le message en une seule fois sur la socket */
  /* pour eviter au maximun de passer dans le select plusieur fois par message du protocole Ivy */
  /* pour eviter la latence ( PB de perfo detecte par ivyperf ping roudtrip ) */

  TRACE( "Send matching args count %d\n",rc);

  for(  indx=1; indx < rc ; indx++ )
    {
      IvyBindingMatch (msg->binding, message, indx, &arglen, & arg );
      if (make_message_var( &bufferArg,  "%.*s" ARG_END , arglen, arg ) < 0) {
	free(bufferArg.data);
	return match_count;
      }
    }
  if (make_message_var( &bufferArg, "\n") < 0) {
    free(bufferArg.data);
    return match_count;
  }

  IVY_LIST_EACH(msg->clientList, clnt ) {
    if (clientUnique != clnt->client)
      continue;
    snprintf (bufferId, sizeof(bufferId), "%d %d" ARG_START ,Msg, clnt->id);
    state = SocketSendRawWithId(clnt->client, bufferId, bufferArg.data , bufferArg.offset);
    match_count++;
    IvyAppCallbackQueueAddSendState(callbacks, clnt, state);
  }
  free(bufferArg.data);
  return match_count;
}



static RWIvyClientPtr CheckConnected( Client sclnt )
{
  RWIvyClientPtr iclient;
  struct sockaddr_storage* addr1;
  struct sockaddr_storage* addr2;
  unsigned short remoteport;

  remoteport = SocketGetRemotePort( sclnt );

  if ( remoteport == 0 ) /* Old Ivy Protocol Dont check */
    return 0;

  IVY_LIST_EACH( allClients, iclient )
    {
      /* client different mais port identique */
      if ((iclient->client != sclnt) && (remoteport == iclient->app_port))
      {
				int same_addr = 0;
				/* et meme machine */
				addr1 = SocketGetRemoteAddr( iclient->client );
				addr2 = SocketGetRemoteAddr( sclnt );
				if ( ipv6 )
				{
					same_addr = memcmp( &((struct sockaddr_in6 *)addr1)->sin6_addr,  &( (struct sockaddr_in6 *)addr2)->sin6_addr, sizeof( struct in6_addr) )== 0  ;
				}
				else
				{
					same_addr = ( (struct sockaddr_in *)addr1)->sin_addr.s_addr == ( (struct sockaddr_in *)addr2)->sin_addr.s_addr;
				}
				if ( same_addr )
				{
					TRACE ("DBG> CheckConnected "
						"clnt->app_uuid[%s] et iclient->app_uuid[%s] %s\n",
						SocketGetUuid (sclnt),
						iclient->app_name,
						SocketGetUuid (iclient->client));
					return iclient;
				}
      }
    }

  return 0;
}



static void Receive( Client client, const void *data, char *line )
{
	IvyContext *ctx = IvyContextFromSocketClient(client);
	IvyContext *previous_ctx = IvyPushCurrentContext(ctx);
	RWIvyClientPtr clnt;
	RWIvyClientPtr other;
	int err,id;
	MsgRcvPtr rcv;
	int argc = 0;
	char *argv[MAX_MATCHING_ARGS];
	char *arg;
	int kind_of_msg = Bye;
	MsgCallback msg_callback = NULL;
	void *msg_user_data = NULL;

	clnt = (RWIvyClientPtr) data;
	if ( clnt->ignore_subsequent_msg ) {
		IvyPopCurrentContext(previous_ctx);
		return;
	}
	err = sscanf( line ,"%d %d", &kind_of_msg, &id );
	arg = strstr( line , ARG_START );
	if ( (err != 2) || (arg == 0)  )
		{
		  SendState error_state;
		  SendState bye_state;
		  clnt->client= client;
		  printf("Quitting bad format  %s\n",  line);
		  error_state = MsgSendTo(clnt, Error, Error, "bad format request expected "
						"'type id ...'" );
		  bye_state = MsgSendTo(clnt, Bye, 0, "" );
		  IvyDispatchSendStateCallback(ctx, clnt, error_state);
		  IvyDispatchSendStateCallback(ctx, clnt, bye_state);
		  SocketClose( client );
		  IvyPopCurrentContext(previous_ctx);
		  return;
		}
	arg++;
	clnt->id = id;
	switch( kind_of_msg )
		{
		case Bye:

			TRACE("Quitting  %s\n",  line);

			SocketClose( client );
			break;
		case Error:
			printf ("Received error %d %s\n",  id, arg);
			break;
		case AddRegexp:
		{
			IvyBindEvent bind_event;


			TRACE("Regexp  id=%d exp='%s'\n",  id, arg);
			if ( !IvyBindingFilter( arg ) )
				{

				TRACE("Warning: regexp '%s' filtered, removing from %s\n",arg,ApplicationName);

				IvyDispatchBindCallback(ctx, clnt, id, arg, IvyFilterBind);
				IvyPopCurrentContext(previous_ctx);
				return;
				}

			IvyBindingsWriteLock(ctx);
			bind_event = addOrChangeRegexp (arg, clnt);
			IvyBindingsWriteUnlock(ctx);
			IvyDispatchBindCallback(ctx, clnt, id, arg, bind_event);
			break;
		}
		case DelRegexp:

		  TRACE("Regexp Delete id=%d\n",  id);
		  IvyBindingsWriteLock(ctx);
		  if (delRegexpForOneClient (clnt, id)) {
		    IvyBindingsWriteUnlock(ctx);
		    IvyDispatchBindCallback(ctx, clnt, id, arg, IvyRemoveBind);
		  } else {
		    IvyBindingsWriteUnlock(ctx);
		  }
		  break;
		case StartRegexp:

			TRACE("Regexp Start id=%d Application='%s'\n",  id, arg);

#ifdef OPENMP
			clnt->endRegexpReceived=0;
#endif // OPENMP

			IvyBindingsWriteLock(ctx);
			clnt->app_name = strdup( arg );
			clnt->app_port = id;
			other =  CheckConnected(  clnt->client );
			if ( other )
			{
				RWIvyClientPtr target;
				// Dilemma choose the rigth client to close
        // the symetric processing will try to close each other
        // only one side may be closed
        unsigned short int other_localPort, other_remotePort, clnt_localPort, clnt_remotePort;

         other_localPort 	= SocketGetLocalPort( other->client);
         other_remotePort = SocketGetRemotePort( other->client);
         clnt_localPort 	= SocketGetLocalPort( clnt->client);
         clnt_remotePort	= SocketGetRemotePort( clnt->client);

        if (MAXPORT(other_localPort, other_remotePort) > MAXPORT( clnt_localPort, clnt_remotePort ))
                {
                    target = other;
                    //printf("choose %s other ports %d,%d\n", target->app_name, other_localPort, other_remotePort);
                }
                else
                {
                    target = clnt;
                    //printf("choose %s this ports %d,%d\n", target->app_name, clnt_remotePort, clnt_localPort);
                }


				TRACE("Quitting already connected %s\n",  line);
				printf("Receive StartRegexp: Quitting already connected %s\n",  line);

				(void)MsgSendTo(target, Error, 0, "Application already connected");
				SocketClose( target->client );
				target->ignore_subsequent_msg = 1;
			}
			IvyBindingsWriteUnlock(ctx);
			break;
		case EndRegexp:
		{
			int send_ready_message;

			TRACE("Regexp End id=%d\n",  id);
			IvyDispatchApplicationCallback(ctx, clnt, IvyApplicationConnected);

			IvyBindingsWriteLock(ctx);
#ifdef OPENMP
			clnt->endRegexpReceived=1;
			regenerateRegPtrArrayCache();
#endif // OPENMP
			clnt->readyToSend++;
			send_ready_message = ready_message && clnt->readyToSend == 2;
			IvyBindingsWriteUnlock(ctx);
			if ( send_ready_message )
				{
				  /* int count = */ ClientCall( clnt, ready_message );
				// count = IvySendMsg ("%s", ready_message );
				// printf ("%s sending READY MESSAGE %d\n", clnt->app_name, count);
				}
			break;
		}
		case Msg:

			TRACE("Message id=%d msg='%s'\n", id, arg);

			IvyBindingsReadLock(ctx);
			IVY_LIST_EACH( msg_recv, rcv )
				{
				if ( id == rcv->id )
					{
					arg = nextArg( arg, ARG_END);
					while ( arg )
						{
						argv[argc++] = arg;
						arg = nextArg( 0, ARG_END );
						}
					TRACE("Calling  id=%d argc=%d for %s\n", id, argc,rcv->regexp);
					msg_callback = rcv->callback;
					msg_user_data = rcv->user_data;
					IvyBindingsReadUnlock(ctx);
					if ( msg_callback ) {
						IvyCallbackEnter(ctx);
						(*msg_callback)( clnt, msg_user_data, argc, argv );
						IvyCallbackLeave(ctx);
					}
					IvyPopCurrentContext(previous_ctx);
					return;
					}
				}
			IvyBindingsReadUnlock(ctx);
			printf("Callback Message id=%d not found!!!'\n", id);
			break;
			case DirectMsg:

				TRACE("Direct Message id=%d msg='%s'\n", id, arg);

				IvyDispatchDirectCallback(ctx, clnt, id, arg);
				break;

			case Die:

				TRACE("Die Message\n");

				IvyDispatchDieCallback(ctx, clnt, id);
				IvyCleanup();
				//exit(0);
				IvyContextStop(ctx); // quit properly the mainloop instead of wildly exit the process
				break;

		case Ping:

			TRACE("Ping Message\n");
			IvyDispatchSendStateCallback(ctx, clnt, MsgSendTo(clnt, Pong, id, "" ));
			break;

		case Pong:

			TRACE("Pong Message\n");
			if (IvyHasPongCallback(ctx)) {
			  if (timerisset (&(clnt->ping_timestamp.ts))) {
			    struct timeval now, diff;
			    int roundTripOrTimout;
			    gettimeofday (&now, NULL);
			    timersub (&now, &(clnt->ping_timestamp.ts), &diff);
			    roundTripOrTimout = (MIN(diff.tv_sec, 2000) *1000000) + (diff.tv_usec);
			    timerclear (&(clnt->ping_timestamp.ts));

			    // if received id is not the last sent id, it means that whe have not
			    // received the pong of previous ping, so we send negative value which mean
			    // this value is a timout
			    if (id != clnt->ping_timestamp.id) {
			      roundTripOrTimout *= -1;
			    }
			    IvyDispatchPongCallback(ctx, clnt, roundTripOrTimout);
			  }
			} else {
			  fprintf(stderr, "Receive unhandled Pong message (no registered pong callback defined)\n");
			}
			break;

		default:
			printf("Receive unhandled message %s\n",  line);
			break;
		}
	IvyPopCurrentContext(previous_ctx);

}

static RWIvyClientPtr SendService( Client client, const char *appname )
{
	IvyContext *ctx = IvyGetCurrentContext();
	RWIvyClientPtr clnt;
	MsgRcvPtr msg;
	int send_ready_message;
	IvyAppCallbackQueue callback_events;
	IvyAppCallbackQueueInit(&callback_events);
	IvyBindingsWriteLock(ctx);
	IVY_LIST_ADD_START( allClients, clnt )
		clnt->client = client;
		clnt->app_name = strdup(appname);
		clnt->app_port = 0;
		clnt->readyToSend = 0;
		clnt->ignore_subsequent_msg =0;
		clnt->ping_timestamp.ts.tv_sec = clnt->ping_timestamp.ts.tv_usec = 0;
		clnt->ping_timestamp.id=0;
		IvyAppCallbackQueueAddSendState(&callback_events, clnt,
			MsgSendTo(clnt, StartRegexp, ApplicationPort, ApplicationName));
		IVY_LIST_EACH(msg_recv, msg )
			{
			  IvyAppCallbackQueueAddSendState(&callback_events, clnt,
				  MsgSendTo(clnt, AddRegexp,msg->id,msg->regexp));
			}
		IvyAppCallbackQueueAddSendState(&callback_events, clnt,
			MsgSendTo(clnt, EndRegexp, 0, ""));

	IVY_LIST_ADD_END( allClients, clnt )

	clnt->readyToSend++;
	send_ready_message = ready_message && clnt->readyToSend == 2;
	  //printf ("DBG> SendService addAllClient: name=%s; client->client=%p\n", appname, clnt->client);

	IvyBindingsWriteUnlock(ctx);
	IvyAppCallbackQueueDispatch(ctx, &callback_events);
	IvyAppCallbackQueueDestroy(&callback_events);
	if ( send_ready_message )
				{
				  /* int count = */ ClientCall( clnt, ready_message );
				// count = IvySendMsg ("%s", ready_message );
				// printf ("%s sending READY MESSAGE %d\n", clnt->app_name, count);
				}
	return clnt;
}

static void ClientDelete( Client client, const void *data )
{
	IvyContext *ctx = IvyContextFromSocketClient(client);
	IvyContext *previous_ctx = IvyPushCurrentContext(ctx);
	IvyClientPtr clnt;

#ifdef DEBUG
	const char *remotehost;
	unsigned short remoteport;
#endif
	clnt = (IvyClientPtr)data;
	IvyDispatchApplicationCallback(ctx, clnt, IvyApplicationDisconnected);

#ifdef DEBUG
	/* probably bogus call, but this is for debug only anyway */
	SocketGetRemoteHost( client, &remotehost, &remoteport );
	TRACE("Deconnexion de %s:%hu\n", remotehost, remoteport );
#endif /*DEBUG */
	IvyBindingsWriteLock(ctx);
	delOneClient (client);
	IvyBindingsWriteUnlock(ctx);
	IvyPopCurrentContext(previous_ctx);
}

static void ClientDecongestion ( Client client, const void *data )
{
  IvyContext *ctx = IvyContextFromSocketClient(client);
  IvyContext *previous_ctx = IvyPushCurrentContext(ctx);
  IvyClientPtr clnt;

#ifdef DEBUG
  const char *remotehost;
  unsigned short remoteport;
#endif
  clnt = (IvyClientPtr)data;
  IvyDispatchApplicationCallback(ctx, clnt, IvyApplicationDecongestion);

#ifdef DEBUG
  /* probably bogus call, but this is for debug only anyway */
  SocketGetRemoteHost( client, &remotehost, &remoteport );
  TRACE("Decongestion de %s:%hu\n", remotehost, remoteport );
#endif /*DEBUG */
  IvyPopCurrentContext(previous_ctx);
}



static void *ClientCreate( Client client )
{
  IvyContext *ctx = IvyContextFromSocketClient(client);
  IvyContext *previous_ctx = IvyPushCurrentContext(ctx);

  char appName[64];
  const char *remotehost;
  unsigned short remoteport;

  SocketGetRemoteHost( client, &remotehost, &remoteport );
  snprintf (appName, sizeof (appName), " %s:%hu", remotehost, remoteport);
  // #ifdef DEBUG

  TRACE ("%s : Connexion de %s client=%p\n", ApplicationName, appName, client);
  // #endif /*DEBUG */

  if (CheckConnected (client))  {
    TRACE  ("Quitting already connected %s\n",  appName);
    printf ("ClientCreate Quitting already connected %s\n",  appName);
    //    IvySendError( client, 0, "Application already connected" );
    SocketClose( client );
    IvyPopCurrentContext(previous_ctx);
    return (NULL);
  }

  {
    RWIvyClientPtr created = SendService (client, appName);
    IvyPopCurrentContext(previous_ctx);
    return created;
  }
}



static void BroadcastReceive( Client client, const void *data, char *line )
{
	IvyContext *ctx = IvyContextFromSocketClient(client);
	IvyContext *previous_ctx = IvyPushCurrentContext(ctx);
	Client app=NULL;
	int err;
	int version;
	unsigned short serviceport;
	char appid[128];
	char appname[2048];
	unsigned short remoteport;
	const char *remotehost = 0;

	memset( appid, 0, sizeof( appid ) );
	memset( appname, 0, sizeof( appname ) );
	err = sscanf (line,"%d %hu %127s %2047[^\n]", &version, &serviceport, appid, appname);
	if ( err < 2 ) {
		/* ignore the message */
		SocketGetRemoteHost (client, &remotehost, &remoteport );
		printf (" Bad supervision message, expected 'version port' from %s:%d\n",
				remotehost, remoteport);
		IvyPopCurrentContext(previous_ctx);
		return;
	}
	if ( version != IVYMAJOR_VERSION ) {
		/* ignore the message */
		SocketGetRemoteHost (client, &remotehost, &remoteport );
		fprintf (stderr, "Bad Ivy version, expected %d and got %d from %s:%d\n",
			IVYMAJOR_VERSION, version, remotehost, remoteport);
		IvyPopCurrentContext(previous_ctx);
		return;
	}
	/* check if we received our own message. SHOULD ALSO TEST THE HOST */
	if ( strcmp( appid , ApplicationID) ==0 ) {
		IvyPopCurrentContext(previous_ctx);
		return;
	}
	//	if (serviceport == ApplicationPort) return;

#ifdef DEBUG
	SocketGetRemoteHost (client, &remotehost, &remoteport );
	TRACE(" Broadcast de %s:%hu port %hu\n", remotehost, remoteport, serviceport );
#endif /*DEBUG */

	/* connect to the service and send the regexp */
	app = SocketConnectAddrFor(ctx->ivy_sockets, ipv6,SocketGetRemoteAddr(client), serviceport, 0, Receive,
				ClientDelete, ClientDecongestion );
	if (app) {
		IvyClientPtr clnt;
		clnt = SendService( app, appname );
		SocketSetData( app, clnt);
		SocketSetUuid (clnt->client, appid);

	} else {
	  printf ("SocketConnectAddr error .....\n");
	  SocketSetData( app, NULL);
	}
	IvyPopCurrentContext(previous_ctx);
}
static unsigned long currentTime()
{
#define MILLISEC 1000
	unsigned long current;
#ifdef WIN32
	current = GetTickCount();
#else
	struct timeval stamp;
        gettimeofday( &stamp, NULL );
        current = stamp.tv_sec * MILLISEC + stamp.tv_usec/MILLISEC;
#endif
        return  current;
}

static const char * GenApplicationUniqueIdentifier(IvyContext *ctx)
{
	unsigned long curtime;
	curtime = currentTime();
	srand( curtime );
	snprintf(ctx->ivy_application_id_buffer, sizeof (ctx->ivy_application_id_buffer),
		 "%d:%lu:%d", rand(), curtime, ctx->ivy_application_port);
	return ctx->ivy_application_id_buffer;
}

int IvyInit (const char *appname, const char *ready,
			 IvyApplicationCallback callback, void *data,
			 IvyDieCallback die_callback, void *die_data
			 )
{
	IvyContext *ctx = IvyGetCurrentContext();
	char *new_appname = NULL;
	char *new_ready = NULL;

	if (IvyContextStateIs(ctx, IVY_CTX_RUNNING) ||
	    IvyContextStateIs(ctx, IVY_CTX_STARTING) ||
	    IvyContextStateIs(ctx, IVY_CTX_STOPPING))
		return IvyReturnStatus(IVY_ESTATE);

	if (appname) {
		new_appname = strdup(appname);
		if (!new_appname)
			return IvyReturnStatus(IVY_ENOMEM);
	}
	if (ready) {
		new_ready = strdup(ready);
		if (!new_ready) {
			free(new_appname);
			return IvyReturnStatus(IVY_ENOMEM);
		}
	}

	SocketInitFor(ctx->ivy_sockets);
	free(ctx->ivy_application_name);
	ctx->ivy_application_name = new_appname;
	ctx->ivy_application_callback = callback;
	ctx->ivy_application_user_data = data;
	ctx->ivy_application_die_callback = die_callback;
	ctx->ivy_application_die_user_data = die_data;
	free(ctx->ivy_ready_message);
	ctx->ivy_ready_message = new_ready;
	IvyContextSetState(ctx, IVY_CTX_CREATED);

	if ( getenv( "IVY_DEBUG_BINARY" )) ctx->ivy_debug_binary_msg = 1;
	return IvyReturnStatus(IVY_OK);
}
int IvyTerminate()
{
	int status = IVY_OK;
	IvyContext *ctx = default_ctx;
	if (ctx) {
	  status = IvyContextDestroy(ctx);
	  if (status == IVY_OK) {
	    if (ivy_current_context == ctx)
	      IvySetCurrentContext(NULL);
	    default_ctx = NULL;
	  }
	}
	IvyBindingTerminate();
	if (status != IVY_OK)
		return status;
	return IvyReturnStatus(IVY_OK);
}

int IvyContextSetBindCallback(IvyContext *ctx, IvyBindCallback bind_callback, void *bind_data )
{
  int status = IvyContextRejectIfStopped(ctx);
  if (status != IVY_OK)
    return status;
  IvyMutexLock(&ctx->ivy_mutex);
  ctx->ivy_application_bind_callback = bind_callback;
  ctx->ivy_application_bind_data = bind_data;
  IvyMutexUnlock(&ctx->ivy_mutex);
  return IvyReturnStatus(IVY_OK);
}

int IvySetBindCallback( IvyBindCallback bind_callback, void *bind_data )
{
  return IvyContextSetBindCallback(IvyGetCurrentContext(), bind_callback, bind_data);
}

int IvyContextSetPongCallback(IvyContext *ctx, IvyPongCallback pong_callback )
{
  int status = IvyContextRejectIfStopped(ctx);
  if (status != IVY_OK)
    return status;
  IvyMutexLock(&ctx->ivy_mutex);
  ctx->ivy_application_pong_callback = pong_callback;
  IvyMutexUnlock(&ctx->ivy_mutex);
  return IvyReturnStatus(IVY_OK);
}

int IvySetPongCallback( IvyPongCallback pong_callback )
{
  return IvyContextSetPongCallback(IvyGetCurrentContext(), pong_callback);
}

int IvySetFilter( int argc, const char **argv)
{
	int status = IvyContextRejectIfStopped(IvyGetCurrentContext());
	if (status != IVY_OK)
		return status;
	IvyBindingSetFilter( argc, argv );
	if ( getenv( "IVY_DEBUG_FILTER" )) debug_filter = 1;
	return IvyReturnStatus(IVY_OK);
}
int IvyAddFilter( const char *arg)
{
	int status = IvyContextRejectIfStopped(IvyGetCurrentContext());
	if (status != IVY_OK)
		return status;
	IvyBindingAddFilter( arg );
	if ( getenv( "IVY_DEBUG_FILTER" )) debug_filter = 1;
	return IvyReturnStatus(IVY_OK);
}
int IvyRemoveFilter( const char *arg)
{
	int status = IvyContextRejectIfStopped(IvyGetCurrentContext());
	if (status != IVY_OK)
		return status;
	IvyBindingRemoveFilter( arg );
	return IvyReturnStatus(IVY_OK);
}

static void IvyContextStopInLoop(void *data)
{
	IvyContext *ctx = (IvyContext *)data;

	if (!ctx)
		return;

	IvyMutexLock(&ctx->ivy_mutex);
	if (ctx->ivy_state != IVY_CTX_STOPPED) {
		ctx->ivy_state = IVY_CTX_STOPPING;
		IvyChannelStopFor(ctx->ivy_loop);
		ctx->ivy_state = IVY_CTX_STOPPED;
		IvyCondBroadcast(&ctx->ivy_stop_done);
	}
	IvyMutexUnlock(&ctx->ivy_mutex);
}

int IvyContextStop(IvyContext *ctx)
{
	IvyContextState state;
	int should_post = 0;
	int should_wait = 0;
	int run_direct = 0;

	if (!ctx)
		return IvyReturnStatus(IVY_EINVAL);

	IvyMutexLock(&ctx->ivy_mutex);
	state = ctx->ivy_state;
	if (state == IVY_CTX_STOPPED) {
		IvyMutexUnlock(&ctx->ivy_mutex);
		return IvyReturnStatus(IVY_OK);
	}

	if (state != IVY_CTX_RUNNING && state != IVY_CTX_STARTING && state != IVY_CTX_STOPPING) {
		ctx->ivy_state = IVY_CTX_STOPPED;
		IvyCondBroadcast(&ctx->ivy_stop_done);
		IvyMutexUnlock(&ctx->ivy_mutex);
		return IvyReturnStatus(IVY_OK);
	}

	if (state != IVY_CTX_STOPPING) {
		ctx->ivy_state = IVY_CTX_STOPPING;
		should_post = 1;
	}

#ifdef WIN32
	run_direct = 1;
#else
	if (IvyChannelIsLoopThreadFor(ctx->ivy_loop) || !IvyChannelLoopIsActiveFor(ctx->ivy_loop))
		run_direct = 1;
	else
		should_wait = 1;
#endif

	IvyMutexUnlock(&ctx->ivy_mutex);

	if (run_direct) {
		IvyContextStopInLoop(ctx);
		return IvyReturnStatus(IVY_OK);
	}

	if (should_post && IvyChannelPostControlFor(ctx->ivy_loop, IvyContextStopInLoop, ctx) != 0) {
		IvyContextStopInLoop(ctx);
		return IvyReturnStatus(IVY_OK);
	}

	if (should_wait) {
		IvyMutexLock(&ctx->ivy_mutex);
		while (ctx->ivy_state != IVY_CTX_STOPPED)
			IvyCondWait(&ctx->ivy_stop_done, &ctx->ivy_mutex);
		IvyMutexUnlock(&ctx->ivy_mutex);
	}

	return IvyReturnStatus(IVY_OK);
}

int IvyStop (void)
{
	return IvyContextStop(IvyGetCurrentContext());
}

void IvyContextMainLoop(IvyContext *ctx)
{
	if (!ctx)
		return;
	IvyMainLoopFor(ctx->ivy_loop);
}

void IvyContextIdle(IvyContext *ctx)
{
	if (!ctx)
		return;
	IvyIdleFor(ctx->ivy_loop);
}

static int ParseIvyIPv4Broadcast(const char *start, const char *end, uint32_t *out)
{
	uint32_t mask = UINT32_MAX;
	uint32_t elem = 0;
	int numdigit = 0;
	int numelem = 0;
	const char *p;

	if (!start || !end || !out || start == end)
		return 0;

	for (p = start; ; p++) {
		const int c = (p < end) ? (unsigned char)*p : '\0';

		if (isdigit(c)) {
			if (numdigit >= 3 || numelem >= 4)
				return 0;
			elem = 10u * elem + (uint32_t)(c - '0');
			numdigit++;
			if (elem > 255u)
				return 0;
		} else if (c == '.' || c == '\0') {
			if (numdigit == 0 || numelem >= 4)
				return 0;

			const uint32_t shift = 8u * (uint32_t)(3 - numelem);
			mask = (mask ^ (0xffu << shift)) | (elem << shift);
			if (c == '\0') {
				*out = mask;
				return 1;
			}

			numelem++;
			numdigit = 0;
			elem = 0;
		} else if (c != ' ') {
			return 0;
		}
	}
}

int IvyContextStart(IvyContext *ctx, const char* bus)
{
	struct in6_addr ipv6addr;
	struct in_addr baddr;
	int error = 0;
	const char* p = bus;	/* used for decoding address list */
	const char* q;			/* used for decoding port number */
	char addr[1024] = "";	/* used for decoding addr */

	if (!ctx)
		return IvyReturnStatus(IVY_EINVAL);

	if (!IvyContextStateIs(ctx, IVY_CTX_CREATED))
		return IvyReturnStatus(IVY_ESTATE);

	IvyContextSetState(ctx, IVY_CTX_STARTING);
	IvyMutexLock(&ctx->ivy_mutex);
	ctx->ivy_owner_thread = IvyThreadCurrent();
	ctx->ivy_owner_thread_set = 1;
	IvyMutexUnlock(&ctx->ivy_mutex);
	ctx->ivy_ipv6 = 0;


	/*
	 * Find network list as well as broadcast port
	 * (we accept things like 123.231,123.123:2000 or 123.231 or :2000),
	 * Initialize UDP port
	 * Send a broadcast handshake on every network
	 */

	/* first, let's find something to parse */
	if (!p || !*p)
		p = getenv ("IVYBUS");
	if (!p || !*p)
		p = DefaultIvyBus;

	/* then, let's get a port number */
	q = strrchr (p, ':');
	if (q)
	{
		char *endptr = NULL;
		unsigned long parsed_port;

		errno = 0;
		parsed_port = strtoul(q + 1, &endptr, 10);
		if (errno == 0 && endptr != q + 1 && *endptr == '\0' &&
		    parsed_port > 0 && parsed_port <= 65535UL) {
			size_t addr_len = (size_t)(q - p);

			if (addr_len >= sizeof(addr)) {
				fprintf(stderr, "Ivy bus address too long\n");
				IvyContextSetState(ctx, IVY_CTX_CREATED);
				return IvyReturnStatus(IVY_EINVAL);
			}
			ctx->ivy_supervision_port = (unsigned short)parsed_port;
			memcpy(addr, p, addr_len);
			addr[addr_len] = '\0';
		} else {
			ctx->ivy_supervision_port = IVY_DEFAULT_BUS;
		}
	}
	else
		ctx->ivy_supervision_port = IVY_DEFAULT_BUS;

	/* test IPV6 mode */

	error =  inet_pton(AF_INET6, addr, &ipv6addr);
	if ( error ==1 )
	{
		ctx->ivy_ipv6 = 1 ;
		printf("Ivy Using IPV6 mode\n");
	}
	/*
	 * Initialize TCP port
	 */
	SocketInitFor(ctx->ivy_sockets);
	ctx->ivy_server = SocketServerFor (ctx->ivy_sockets, ctx->ivy_ipv6, ANYPORT, ClientCreate, ClientDelete,
			       ClientDecongestion, Receive);
	if (!ctx->ivy_server) {
		IvyContextSetState(ctx, IVY_CTX_CREATED);
		return IvyReturnStatus(IVY_EIO);
	}
	ctx->ivy_application_port = SocketServerGetPort (ctx->ivy_server);
	ctx->ivy_application_id = GenApplicationUniqueIdentifier(ctx);


	/*
	 * Now we have a port number it's time to initialize the UDP port
	 */
	ctx->ivy_broadcast =  SocketBroadcastCreateFor (ctx->ivy_sockets, ctx->ivy_ipv6, ctx->ivy_supervision_port, 0, BroadcastReceive );


	/* then, if we only have a port number, resort to default value for network */
	if (p == q)
		p = DefaultIvyBus;

	if ( ctx->ivy_ipv6 )
	{
		char dst[1024];
		const char * bcast_addr = inet_ntop(AF_INET6, &ipv6addr,
			dst, sizeof(dst) );
		if ( bcast_addr )
		{
		printf ("Broadcasting on network %s, port %d\n",
					dst, ctx->ivy_supervision_port);
		/* test mask value agaisnt CLASS D */
		if ( IN6_IS_ADDR_MULTICAST( &ipv6addr ) )
			SocketAddMember6 (ctx->ivy_broadcast ,  &ipv6addr );

		SocketSendBroadcast6 (ctx->ivy_broadcast,  &ipv6addr, ctx->ivy_supervision_port,
				     "%d %hu %s %s\n", IVYMAJOR_VERSION, ctx->ivy_application_port,
				     ctx->ivy_application_id, ctx->ivy_application_name);
		}
	}
	else
	{

	/* and finally, parse network list and send broadcast handshakes.
	   This is painful but inet_aton is sloppy.
	   If someone knows other builtin routines that do that... */
	for (;;) {
		const char *addr_start = p;
		uint32_t mask;

		while (*p && *p != ',' && *p != ':')
			p++;

		if (ParseIvyIPv4Broadcast(addr_start, p, &mask)) {
				baddr.s_addr = htonl(mask);
				printf ("Broadcasting on network %s, port %d\n",
					inet_ntoa(baddr), ctx->ivy_supervision_port);
				/* test mask value agaisnt CLASS D */
				if ( IN_MULTICAST( mask ) )
					SocketAddMember (ctx->ivy_broadcast , mask );

				SocketSendBroadcast (ctx->ivy_broadcast, mask, ctx->ivy_supervision_port,
						     "%d %hu %s %s\n", IVYMAJOR_VERSION, ctx->ivy_application_port,
						     ctx->ivy_application_id, ctx->ivy_application_name);
		} else {
			fprintf (stderr, "bad broadcast address\n");
		}

		/* end of string or colon */
		if (*p == '\0' || *p == ':')
			break;
		++p;
	}
	}
	TRACE ("Listening on TCP:%hu\n", ctx->ivy_application_port);
	IvyContextSetState(ctx, IVY_CTX_RUNNING);
	return IvyReturnStatus(IVY_OK);

}

int IvyStart (const char* bus)
{
	IvyContext *ctx = IvyGetDefaultContext();
	IvySetCurrentContext(ctx);
	return IvyContextStart(ctx, bus);
}

/* desabonnements */
int
IvyContextUnbindMsg (IvyContext *ctx, MsgRcvPtr msg)
{
	IvyContext *previous_ctx;
	IvyClientPtr clnt;
	IvyAppCallbackQueue callback_events;
	int status = IvyContextRejectIfStopped(ctx);

	if (status != IVY_OK)
		return status;
	if (!msg)
		return IvyReturnStatus(IVY_EINVAL);

	previous_ctx = IvyPushCurrentContext(ctx);
	IvyAppCallbackQueueInit(&callback_events);
	IvyBindingsWriteLock(ctx);
	/* Send to already connected clients */
	IVY_LIST_EACH (allClients, clnt ) {
	  IvyAppCallbackQueueAddSendState(&callback_events, clnt,
		  MsgSendTo( clnt, DelRegexp,msg->id, ""));
	}
	free (msg->regexp);
	msg->regexp = NULL;
	IVY_LIST_REMOVE( msg_recv, msg  );
	IvyBindingsWriteUnlock(ctx);
	IvyAppCallbackQueueDispatch(ctx, &callback_events);
	IvyAppCallbackQueueDestroy(&callback_events);
	IvyPopCurrentContext(previous_ctx);
	return IvyReturnStatus(IVY_OK);
}

int
IvyUnbindMsg (MsgRcvPtr msg)
{
	return IvyContextUnbindMsg(IvyGetCurrentContext(), msg);
}

/* demande de reception d'un message */

static MsgRcvPtr
IvyContextBindMsgV(IvyContext *ctx, MsgCallback callback, void *user_data,
		   const char *fmt_regex, va_list ap)
{
	IvyBuffer *buffer;
	int status = IvyContextRejectIfStopped(ctx);
	IvyClientPtr clnt;
	MsgRcvPtr msg;
	IvyAppCallbackQueue callback_events;

	if (status != IVY_OK)
		return NULL;
	if (!fmt_regex) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}

	buffer = &ctx->ivy_bind_buffer;
	buffer->offset = 0;
	if (make_message( buffer, fmt_regex, ap ) < 0) {
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}

	substituteInterval (buffer);

	IvyAppCallbackQueueInit(&callback_events);
	IvyBindingsWriteLock(ctx);
	/* add Msg to the query list */
	IVY_LIST_ADD_START( msg_recv, msg )
		msg->id = ctx->ivy_recv_id++;
		msg->regexp = strdup(buffer->data);
		msg->callback = callback;
		msg->user_data = user_data;
	IVY_LIST_ADD_END( msg_recv, msg )
	/* Send to already connected clients */
	/* recherche dans la liste des requetes recues de mes clients */
	IVY_LIST_EACH( allClients, clnt ) {
	  IvyAppCallbackQueueAddSendState(&callback_events, clnt,
		  MsgSendTo( clnt, AddRegexp,msg->id,msg->regexp));
	}
	IvyBindingsWriteUnlock(ctx);
	IvyAppCallbackQueueDispatch(ctx, &callback_events);
	IvyAppCallbackQueueDestroy(&callback_events);
	IvySetLastError(IVY_OK);
	return msg;
}

MsgRcvPtr
IvyContextBindMsg(IvyContext *ctx, MsgCallback callback, void *user_data,
		  const char *fmt_regex, ... )
{
	IvyContext *previous_ctx;
	MsgRcvPtr msg;
	va_list ap;

	if (!ctx) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}
	previous_ctx = IvyPushCurrentContext(ctx);
	va_start(ap, fmt_regex);
	msg = IvyContextBindMsgV(ctx, callback, user_data, fmt_regex, ap);
	va_end(ap);
	IvyPopCurrentContext(previous_ctx);
	return msg;
}

MsgRcvPtr
IvyBindMsg (MsgCallback callback, void *user_data, const char *fmt_regex, ... )
{
	MsgRcvPtr msg;
	va_list ap;

	va_start(ap, fmt_regex);
	msg = IvyContextBindMsgV(IvyGetCurrentContext(), callback, user_data, fmt_regex, ap);
	va_end(ap);
	return msg;
}

/* changement de regexp d'un bind existant precedement fait avec IvyBindMsg */
static MsgRcvPtr
IvyContextChangeMsgV(IvyContext *ctx, MsgRcvPtr msg, const char *fmt_regex, va_list ap)
{
	IvyBuffer *buffer;
	int status = IvyContextRejectIfStopped(ctx);
	IvyClientPtr clnt;
	char *new_regexp;
	IvyAppCallbackQueue callback_events;

	if (status != IVY_OK)
		return NULL;
	if (!msg || !fmt_regex) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}

	buffer = &ctx->ivy_change_buffer;
	buffer->offset = 0;
	if (make_message( buffer, fmt_regex, ap ) < 0) {
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}

	substituteInterval (buffer);

	IvyAppCallbackQueueInit(&callback_events);
	IvyBindingsWriteLock(ctx);
	/* change Msg in the query list */
	new_regexp = strdup(buffer->data);
	if (!new_regexp) {
		IvyBindingsWriteUnlock(ctx);
		IvyAppCallbackQueueDestroy(&callback_events);
		IvySetLastError(IVY_ENOMEM);
		return NULL;
	}
	free (msg->regexp);
	msg->regexp = new_regexp;

	/* Send to already connected clients */
	/* recherche dans la liste des requetes recues de mes clients */
	IVY_LIST_EACH( allClients, clnt ) {
	  IvyAppCallbackQueueAddSendState(&callback_events, clnt,
		  MsgSendTo(clnt, AddRegexp,msg->id,msg->regexp));
	}
	IvyBindingsWriteUnlock(ctx);
	IvyAppCallbackQueueDispatch(ctx, &callback_events);
	IvyAppCallbackQueueDestroy(&callback_events);
	IvySetLastError(IVY_OK);
	return msg;
}

MsgRcvPtr
IvyContextChangeMsg(IvyContext *ctx, MsgRcvPtr msg, const char *fmt_regex, ... )
{
	IvyContext *previous_ctx;
	MsgRcvPtr changed;
	va_list ap;

	if (!ctx) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}
	previous_ctx = IvyPushCurrentContext(ctx);
	va_start(ap, fmt_regex);
	changed = IvyContextChangeMsgV(ctx, msg, fmt_regex, ap);
	va_end(ap);
	IvyPopCurrentContext(previous_ctx);
	return changed;
}

MsgRcvPtr
IvyChangeMsg (MsgRcvPtr msg, const char *fmt_regex, ... )
{
	MsgRcvPtr changed;
	va_list ap;

	va_start(ap, fmt_regex);
	changed = IvyContextChangeMsgV(IvyGetCurrentContext(), msg, fmt_regex, ap);
	va_end(ap);
	return changed;
}



static int IvyContextSendMsgV(IvyContext *ctx, const char *fmt, va_list ap) /* version dictionnaire */
{
  int match_count = 0;

#ifndef OPENMP
  MsgSndDictPtr msgSendDict;
#endif
  IvyBuffer buffer = { NULL, 0, 0 };
  int status = IvyContextRejectIfStopped(ctx);
  IvyAppCallbackQueue callback_events;

  if (status != IVY_OK)
    return status;

  /* construction du buffer message à partir du format et des arguments */
  if( fmt == 0 || strlen(fmt) == 0 ) {
    IvySetLastError(IVY_OK);
    return 0;
  }
  buffer.offset = 0;
  if (make_message( &buffer, fmt, ap ) < 0) {
    free(buffer.data);
    return IvyReturnStatus(IVY_ENOMEM);
  }

  /* test du contenu du message */
  if ( debug_binary_msg  )
    {
      if ( IvyCheckBuffer( buffer.data ) ) {
	free(buffer.data);
	IvySetLastError(IVY_OK);
	return 0;
      }
    }

  /*   pour toutes les regexp */
  IvyAppCallbackQueueInit(&callback_events);
  IvyBindingsReadLock(ctx);

#ifdef OPENMP
  {
  MsgSndDictPtr *msg_ptr_array = ompDictCache.msgPtrArray;
  int msg_ptr_count = ompDictCache.numPtr;
  const char *message_data = buffer.data;
  IvyAppCallbackQueue *callback_events_ptr = &callback_events;
#define TABLEAU_PREALABLE 1 // mode normal, les autres sont pour le debug
  //#define TABLEAU_PREALABLE_SEQUENTIEL 1
  //#define SINGLE_NOWAIT  1
  //#define SCHEDULE_GUIDED 1
  //#define SEQUENTIEL_DEBUG 1

#ifdef SCHEDULE_GUIDED
  int count;
#pragma omp parallel  default(none) private(count) shared(msg_ptr_array, msg_ptr_count, message_data, callback_events_ptr) \
                      reduction(+:match_count)
  {
#pragma omp for schedule(guided) // après debug mettre  schedule(guided, 10)
  for(count=0; count<msg_ptr_count; count++) {
		match_count += RegexpCall (msg_ptr_array[count], message_data, callback_events_ptr);
		}
  }
#endif // SCHEDULE_GUIDED


#ifdef  TABLEAU_PREALABLE
  int count; // PARALLEL FOR
#pragma omp parallel for default(none) private(count) shared(msg_ptr_array, msg_ptr_count, message_data, callback_events_ptr) \
			 reduction(+:match_count)
  for(count=0; count<msg_ptr_count; count++) {
    match_count += RegexpCall (msg_ptr_array[count], message_data, callback_events_ptr);
  }
#endif // TABLEAU_PREALABLE


#ifdef  TABLEAU_PREALABLE_SEQUENTIEL
  int count;
  for(count=0; count<msg_ptr_count; count++) {
    match_count += RegexpCall (msg_ptr_array[count], message_data, callback_events_ptr);
  }
#endif // TABLEAU_PREALABLE_SEQUENTIEL


#ifdef SINGLE_NOWAIT // OPEMMP LISTE
  MsgSndDictPtr msgSendDict;
#pragma omp parallel  default(shared)  private(msgSendDict) reduction(+:match_count)
  for (msgSendDict=messSndByRegexp; msgSendDict ; msgSendDict=msgSendDict->hh.next) {
#pragma omp single nowait
    match_count += RegexpCall (msgSendDict, message_data, callback_events_ptr);
  }
#endif // SINGLE_NOWAIT

#ifdef SEQUENTIEL_DEBUG // OPEMMP LISTE
  MsgSndDictPtr msgSendDict;

  for (msgSendDict=messSndByRegexp; msgSendDict ; msgSendDict=msgSendDict->hh.next) {
    match_count += RegexpCall (msgSendDict, message_data, callback_events_ptr);
  }
#endif // SEQUENTIEL_DEBUG

  }

#else // PAS OPENMP

  for (msgSendDict=messSndByRegexp; msgSendDict ; msgSendDict=(MsgSndDictPtr) msgSendDict->hh.next) {
    match_count += RegexpCall (msgSendDict, buffer.data, &callback_events);
  }
#endif
  IvyBindingsReadUnlock(ctx);
  IvyAppCallbackQueueDispatch(ctx, &callback_events);
  IvyAppCallbackQueueDestroy(&callback_events);

  TRACE_IF( match_count == 0, "Warning no recipient for %s\n",buffer.data);
  /* si le message n'est pas emit et qu'il y a des filtres alors WARNING */
  if ( match_count == 0 && debug_filter )
    {
      IvyBindindFilterCheck( buffer.data );
    }
  free(buffer.data);
  IvySetLastError(IVY_OK);
  return match_count;
}

int IvyContextSendMsg(IvyContext *ctx, const char *fmt, ...)
{
  IvyContext *previous_ctx;
  int result;
  va_list ap;

  if (!ctx)
    return IvyReturnStatus(IVY_EINVAL);
  previous_ctx = IvyPushCurrentContext(ctx);
  va_start(ap, fmt);
  result = IvyContextSendMsgV(ctx, fmt, ap);
  va_end(ap);
  IvyPopCurrentContext(previous_ctx);
  return result;
}

int IvySendMsg(const char *fmt, ...)
{
  int result;
  va_list ap;

  va_start(ap, fmt);
  result = IvyContextSendMsgV(IvyGetCurrentContext(), fmt, ap);
  va_end(ap);
  return result;
}


/* teste de la presence de binaire dans les message Ivy */
static int IvyCheckBuffer( const char* buffer )
{
	const char * ptr = buffer;
	while ( *ptr )
	{
		if ( *ptr++ < ' ' )
		{
			fprintf(stderr," IvySendMsg bad msg to send binary data not allowed ignored %s\n",
				buffer );
			return 1;
		}
	}
	return 0;
}


static int IvyContextSendErrorV(IvyContext *ctx, IvyClientPtr app, int id,
				const char *fmt, va_list ap)
{
	IvyBuffer buffer = { NULL, 0, 0 };
	int status = IvyContextRejectIfStopped(ctx);
	SendState send_state;

	if (status != IVY_OK)
		return status;
	if (!app || !fmt)
		return IvyReturnStatus(IVY_EINVAL);

	buffer.offset = 0;
	if (make_message( &buffer, fmt, ap ) < 0) {
		free(buffer.data);
		return IvyReturnStatus(IVY_ENOMEM);
	}
	send_state = MsgSendTo(app, Error, id, buffer.data);
	free(buffer.data);
	IvyDispatchSendStateCallback(ctx, app, send_state);
	return IvyReturnStatus(IvyStatusFromSendState(send_state));
}

int IvyContextSendError(IvyContext *ctx, IvyClientPtr app, int id, const char *fmt, ... )
{
	IvyContext *previous_ctx;
	int result;
	va_list ap;

	if (!ctx)
		return IvyReturnStatus(IVY_EINVAL);
	previous_ctx = IvyPushCurrentContext(ctx);
	va_start(ap, fmt);
	result = IvyContextSendErrorV(ctx, app, id, fmt, ap);
	va_end(ap);
	IvyPopCurrentContext(previous_ctx);
	return result;
}

int IvySendError(IvyClientPtr app, int id, const char *fmt, ... )
{
	int result;
	va_list ap;

	va_start(ap, fmt);
	result = IvyContextSendErrorV(IvyGetCurrentContext(), app, id, fmt, ap);
	va_end(ap);
	return result;
}

int IvyContextBindDirectMsg(IvyContext *ctx, MsgDirectCallback callback, void *user_data)
{
	int status = IvyContextRejectIfStopped(ctx);
	if (status != IVY_OK)
		return status;
	IvyMutexLock(&ctx->ivy_mutex);
	ctx->ivy_direct_callback = callback;
	ctx->ivy_direct_user_data = user_data;
	IvyMutexUnlock(&ctx->ivy_mutex);
	return IvyReturnStatus(IVY_OK);
}

int IvyBindDirectMsg( MsgDirectCallback callback, void *user_data)
{
	return IvyContextBindDirectMsg(IvyGetCurrentContext(), callback, user_data);
}

int IvyContextSendDirectMsg(IvyContext *ctx, IvyClientPtr app, int id, char *msg )
{
  int status = IvyContextRejectIfStopped(ctx);
  SendState send_state;
  if (status != IVY_OK)
    return status;
  if (!app || !msg)
    return IvyReturnStatus(IVY_EINVAL);
  send_state = MsgSendTo( app, DirectMsg, id, msg);
  IvyDispatchSendStateCallback(ctx, app, send_state);
  return IvyReturnStatus(IvyStatusFromSendState(send_state));
}

int IvySendDirectMsg(IvyClientPtr app, int id, char *msg )
{
  return IvyContextSendDirectMsg(IvyGetCurrentContext(), app, id, msg);
}

int IvyContextSendPing(IvyContext *ctx, IvyClientPtr app)
{
  int status = IvyContextRejectIfStopped(ctx);
  SendState send_state;
  if (status != IVY_OK)
    return status;
  if (!app)
    return IvyReturnStatus(IVY_EINVAL);
  if (IvyHasPongCallback(ctx)) {
    RWIvyClientPtr clnt = (RWIvyClientPtr) app;

    gettimeofday (&(clnt->ping_timestamp.ts), NULL);
    send_state = MsgSendTo( clnt, Ping, ++clnt->ping_timestamp.id, "");
    IvyDispatchSendStateCallback(ctx, app, send_state);
    return IvyReturnStatus(IvyStatusFromSendState(send_state));
  } else {
    fprintf(stderr,"Application: %s useless IvySendPing issued since no pong callback defined\n",
	    IvyGetApplicationName( app ));
    return IvyReturnStatus(IVY_ESTATE);
  }
}

int IvySendPing( IvyClientPtr app)
{
  return IvyContextSendPing(IvyGetCurrentContext(), app);
}

int IvyContextSendDieMsg(IvyContext *ctx, IvyClientPtr app )
{
  int status = IvyContextRejectIfStopped(ctx);
  SendState send_state;
  if (status != IVY_OK)
    return status;
  if (!app)
    return IvyReturnStatus(IVY_EINVAL);
  send_state = MsgSendTo(app, Die, 0, "" );
  IvyDispatchSendStateCallback(ctx, app, send_state);
  return IvyReturnStatus(IvyStatusFromSendState(send_state));
}

int IvySendDieMsg(IvyClientPtr app )
{
  return IvyContextSendDieMsg(IvyGetCurrentContext(), app);
}

static int IvyContextOwnsApplication(IvyContext *ctx, IvyClientPtr app)
{
	IvyClientPtr found;

	if (!ctx || !app)
		return 0;
	IVY_LIST_ITER(ctx->ivy_all_clients, found, found != app);
	return found != NULL;
}

const char *IvyContextGetApplicationName(IvyContext *ctx, IvyClientPtr app)
{
	const char *name = NULL;

	if (!ctx || !app) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}

	IvyBindingsReadLock(ctx);
	if (IvyContextOwnsApplication(ctx, app) && app->app_name)
		name = app->app_name;
	IvyBindingsReadUnlock(ctx);

	if (!name) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}
	IvySetLastError(IVY_OK);
	return name;
}

const char *IvyContextGetApplicationHost(IvyContext *ctx, IvyClientPtr app)
{
	const char *host = NULL;

	if (!ctx || !app) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}

	IvyBindingsReadLock(ctx);
	if (IvyContextOwnsApplication(ctx, app) && app->client)
		host = SocketGetPeerHost(app->client);
	IvyBindingsReadUnlock(ctx);

	if (!host) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}
	IvySetLastError(IVY_OK);
	return host;
}

const char *IvyGetApplicationName(IvyClientPtr app)
{
	const char *name = IvyContextGetApplicationName(IvyGetCurrentContext(), app);
	return name ? name : "Unknown";
}

const char *IvyGetApplicationHost(IvyClientPtr app)
{
	return IvyContextGetApplicationHost(IvyGetCurrentContext(), app);
}

void IvyDefaultApplicationCallback(IvyClientPtr app, void *user_data, IvyApplicationEvent event)
{
	switch ( event )  {
	case IvyApplicationConnected:
		printf("Application: %s ready on %s\n", IvyGetApplicationName( app ), IvyGetApplicationHost(app));
		break;
	case IvyApplicationDisconnected:
		printf("Application: %s bye on %s\n", IvyGetApplicationName( app ), IvyGetApplicationHost(app));
		break;
	case IvyApplicationCongestion:
		printf("Application: %s congestion on %s\n", IvyGetApplicationName( app ), IvyGetApplicationHost(app));
		break;
	case IvyApplicationDecongestion:
		printf("Application: %s  decongestion on %s\n", IvyGetApplicationName( app ), IvyGetApplicationHost(app));
		break;
	case IvyApplicationFifoFull:
		printf("Application: %s  fifo full, msg on %s will be lost until decongestion\n",
		 IvyGetApplicationName( app ), IvyGetApplicationHost(app));
		break;
	default:
		printf("Application: %s unkown event %d\n",IvyGetApplicationName( app ), event);
		break;
	}
}

void IvyDefaultBindCallback(IvyClientPtr app, void *user_data, int id, const char* regexp,  IvyBindEvent event)
{
	switch ( event )  {
	case IvyAddBind:
		printf("Application: %s on %s add regexp %d : %s\n", IvyGetApplicationName( app ), IvyGetApplicationHost(app), id, regexp);
		break;
	case IvyRemoveBind:
		printf("Application: %s on %s remove regexp %d :%s\n", IvyGetApplicationName( app ), IvyGetApplicationHost(app), id, regexp);
		break;
	case IvyFilterBind:
		printf("Application: %s on %s as been filtred regexp %d :%s\n", IvyGetApplicationName( app ), IvyGetApplicationHost(app), id, regexp);
		break;
	case IvyChangeBind:
	        printf("Application: %s on %s change regexp %d : %s\n", IvyGetApplicationName( app ), IvyGetApplicationHost(app), id, regexp);
		break;
		break;
	default:
		printf("Application: %s unkown event %d\n",IvyGetApplicationName( app ), event);
		break;
	}
}

IvyClientPtr IvyContextGetApplication(IvyContext *ctx, char *name)
{
	IvyClientPtr app = 0;
	int status = IvyContextRejectIfStopped(ctx);
	if (status != IVY_OK)
		return NULL;
	if (!ctx || !name) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}
	IvyBindingsReadLock(ctx);
	IVY_LIST_ITER( ctx->ivy_all_clients, app, strcmp(name, app->app_name) != 0 );
	IvyBindingsReadUnlock(ctx);
	IvySetLastError(IVY_OK);
	return app;
}

IvyClientPtr IvyGetApplication( char *name )
{
	return IvyContextGetApplication(IvyGetCurrentContext(), name);
}

static void IvyAppendQueryBuffer(char *buffer, size_t buffer_size,
	size_t *written, size_t *required, const char *value)
{
	size_t len;

	if (!value)
		value = "";
	len = strlen(value);
	if (buffer && buffer_size > 0 && *written < buffer_size - 1) {
		size_t available = buffer_size - 1 - *written;
		size_t copy_len = len < available ? len : available;
		memcpy(buffer + *written, value, copy_len);
		*written += copy_len;
		buffer[*written] = '\0';
	}
	*required += len;
}

char *IvyContextGetApplicationList(IvyContext *ctx, const char *sep)
{
	char *applist;
	IvyClientPtr app;
	int status = IvyContextRejectIfStopped(ctx);
	if (status != IVY_OK)
		return NULL;
	if (!ctx || !sep) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}
	applist = ctx->ivy_application_list; /* TODO remove that ugly Thing */
	applist[0] = '\0';
	IvyBindingsReadLock(ctx);
	IVY_LIST_EACH( ctx->ivy_all_clients, app )
		{
		strcat( applist, app->app_name );
		strcat( applist, sep );
		}
	IvyBindingsReadUnlock(ctx);
	IvySetLastError(IVY_OK);
	return applist;
}

int IvyContextGetApplicationListBuffer(IvyContext *ctx,
	char *buffer, size_t buffer_size, const char *sep)
{
	IvyClientPtr app;
	size_t written = 0;
	size_t required = 0;
	int status = IvyContextRejectIfStopped(ctx);

	if (status != IVY_OK)
		return status;
	if (!ctx || !sep || (!buffer && buffer_size > 0))
		return IvyReturnStatus(IVY_EINVAL);
	if (buffer_size > 0)
		buffer[0] = '\0';

	IvyBindingsReadLock(ctx);
	IVY_LIST_EACH( ctx->ivy_all_clients, app )
		{
		IvyAppendQueryBuffer(buffer, buffer_size, &written, &required, app->app_name);
		IvyAppendQueryBuffer(buffer, buffer_size, &written, &required, sep);
		}
	IvyBindingsReadUnlock(ctx);
	return IvyReturnQueryBufferSize(required, buffer_size);
}

char *IvyGetApplicationList(const char *sep)
{
	return IvyContextGetApplicationList(IvyGetCurrentContext(), sep);
}

int IvyGetApplicationListBuffer(char *buffer, size_t buffer_size, const char *sep)
{
	return IvyContextGetApplicationListBuffer(IvyGetCurrentContext(),
		buffer, buffer_size, sep);
}

char **IvyContextGetApplicationMessages(IvyContext *ctx, IvyClientPtr app)
{
	char **messagelist;
	GlobRegPtr  msg;
	int msgCount= 0;
	int status = IvyContextRejectIfStopped(ctx);
	if (status != IVY_OK)
		return NULL;
	if (!ctx || !app) {
		IvySetLastError(IVY_EINVAL);
		return NULL;
	}
	messagelist = ctx->ivy_application_messages;/* TODO remove that ugly Thing */
	memset( messagelist, 0 , sizeof(ctx->ivy_application_messages) );
	/* recherche dans la liste des requetes recues de ce client */
	IvyBindingsReadLock(ctx);
	IVY_LIST_EACH( app->srcRegList, msg )
	{
	messagelist[msgCount++]= msg->str_regexp;
	if ( msgCount >= MAX_APPLICATION_MESSAGES )
		{
		fprintf(stderr,"Too Much expression(%d) for buffer\n",msgCount);
		break;
		}
	}
	IvyBindingsReadUnlock(ctx);
	IvySetLastError(IVY_OK);
	return messagelist;
}

int IvyContextGetApplicationMessagesBuffer(IvyContext *ctx,
	IvyClientPtr app, char *buffer, size_t buffer_size, const char *sep)
{
	GlobRegPtr msg;
	size_t written = 0;
	size_t required = 0;
	int status = IvyContextRejectIfStopped(ctx);

	if (status != IVY_OK)
		return status;
	if (!ctx || !app || !sep || (!buffer && buffer_size > 0))
		return IvyReturnStatus(IVY_EINVAL);
	if (buffer_size > 0)
		buffer[0] = '\0';

	IvyBindingsReadLock(ctx);
	IVY_LIST_EACH( app->srcRegList, msg )
		{
		IvyAppendQueryBuffer(buffer, buffer_size, &written, &required, msg->str_regexp);
		IvyAppendQueryBuffer(buffer, buffer_size, &written, &required, sep);
		}
	IvyBindingsReadUnlock(ctx);
	return IvyReturnQueryBufferSize(required, buffer_size);
}

char **IvyGetApplicationMessages( IvyClientPtr app )
{
	return IvyContextGetApplicationMessages(IvyGetCurrentContext(), app);
}

int IvyGetApplicationMessagesBuffer(IvyClientPtr app,
	char *buffer, size_t buffer_size, const char *sep)
{
	return IvyContextGetApplicationMessagesBuffer(IvyGetCurrentContext(),
		app, buffer, buffer_size, sep);
}

static void substituteInterval (IvyBuffer *src)
{
  /* pas de traitement couteux s'il n'y a rien à interpoler */
  if (strstr (src->data, "(?I") == NULL) {
    return;
  } else {
    char *curPos;
    char *itvPos;
    IvyBuffer dst = {NULL, 0, 0};
    dst.size = 8192;
    dst.data = (char *) malloc (dst.size);

    curPos = src->data;
    while ((itvPos = strstr (curPos, "(?I")) != NULL) {
      /* copie depuis la position courante jusqu'à l'intervalle */
      int lenCp, min,max;
      char withDecimal;
      lenCp = itvPos-curPos;
      memcpy (&(dst.data[dst.offset]), curPos, lenCp);
      curPos=itvPos;
      dst.offset += lenCp;

      /* extraction des paramètres de l'intervalle */
      sscanf (itvPos, "(?I%d#%d%c", &min, &max, &withDecimal);

      /*      printf ("DBG> substituteInterval min=%d max=%d withDecimal=%d\n",  */
      /*      min, max, (withDecimal != 'i'));    */

      /* generation et copie de l'intervalle */
      regexpGen (&(dst.data[dst.offset]), dst.size-dst.offset, min, max, (withDecimal != 'i'));
      dst.offset = strlen (dst.data);

      /* consommation des caractères décrivant intervalle dans la chaine source */
      curPos = strstr (curPos, ")");
      curPos++;
    }
    strncat (dst.data, curPos, dst.size-dst.offset);
    free (src->data);
    src->data = dst.data;
  }
}


static void freeClient ( RWIvyClientPtr client)
{
  GlobRegPtr srcReg;

  /* on libere la chaine nom de l'appli*/
  if (client->app_name != NULL) {
    free (client->app_name);
    client->app_name = NULL;
    /* on libere la liste des clients */
    IVY_LIST_EACH (client->srcRegList, srcReg) {
      if (srcReg->str_regexp != NULL) {
	free (srcReg->str_regexp);
	srcReg->str_regexp = NULL;
      }
    }
    IVY_LIST_EMPTY (client->srcRegList);
  }
}







static void delRegexpForOneClientFromDictionary (const char *regexp, IvyClientPtr client)
{
  MsgSndDictPtr msgSendDict = NULL;
  //  printf ("DBG> ENTER delRegexpForOneClientFromDictionary clnt=%d, reg='%s'\n", client, regexp);

/*   static int called = 0; */
/*   called ++; */
/*   if ((called %1) == 0) { */
/*     printf ("DBG> delRegexpForOneClientFromDictionary called =%d\n", called); */
/*   } */

  HASH_FIND_STR(messSndByRegexp, regexp, msgSendDict);
  if (msgSendDict != NULL) {
    delOneIvyClientFromDictionaryEntry (msgSendDict, client);
  }
#ifdef OPENMP
  regenerateRegPtrArrayCache ();
#endif
}



static void delOneIvyClientFromDictionaryEntry (MsgSndDictPtr msgSendDict,
						IvyClientPtr client)
{
  RWIvyClientPtr  client_itr, next;



  //    printf ("DBG> delRegexpForOneClientFromDictionary, regexp '%s' found\n", regexp);
  /* la clef est trouvée, on itere sur la liste de client associée */
  IVY_LIST_EACH_SAFE ( msgSendDict->clientList, client_itr, next) {
    /* pour tester 2 IvyClientPtr, on teste la similarité
       des pointeur Client qui doivent être uniques */
    if ((client_itr->client == client->client) && (client_itr->id == client->id)) {
      /* on a trouve le client : on l'enleve */
      free (client_itr->app_name);
      client_itr->app_name = NULL;
      TRACE ("delOneClientFromDictionaryEntry : IVY_LIST_REMOVE\n");
      IVY_LIST_REMOVE (msgSendDict->clientList, client_itr);
    }
  }
  /* si la liste de clients associée à cette regexp est vide */
  if ((msgSendDict->clientList == NULL) ||
      (IVY_LIST_IS_EMPTY (msgSendDict->clientList))) {
    TRACE ("delRegexpForOneClientFromDictionary : IvyBindingFree, free, hash_del\n");
    //printf ("DBG> delRegexpForOneClientFromDictionary : IvyBindingFree, free, hash_del\n");
    /* on efface le binding */
    IvyBindingFree (msgSendDict->binding);
    /* on enlève l'entrée regexp de la table de hash */
    HASH_DEL (messSndByRegexp, msgSendDict);
    /* on efface la clef (regexp source) */
    free (msgSendDict->regexp_src);
    /* on libère la structure */
    free (msgSendDict);
  }
}



static void delOneClientFromDictionaryEntry (MsgSndDictPtr msgSendDict,
					     const Client client)
{
  RWIvyClientPtr  client_itr, next;

    /* la clef est trouvée, on itere sur la liste de client associée */
  IVY_LIST_EACH_SAFE ( msgSendDict->clientList, client_itr, next) {
    /* pour tester 2 IvyClientPtr, on teste la similarité
       des pointeur Client qui doivent être uniques */
    if (client_itr->client == client) {
      /* on a trouve le client : on l'enleve */
      free (client_itr->app_name);
      client_itr->app_name = NULL;
      TRACE ("delOneClientFromDictionaryEntry : IVY_LIST_REMOVE\n");
      IVY_LIST_REMOVE (msgSendDict->clientList, client_itr);
    }
  }
  /* si la liste de clients associée à cette regexp est vide */
  if ((msgSendDict->clientList == NULL) ||
      (IVY_LIST_IS_EMPTY (msgSendDict->clientList))) {
    TRACE ("delRegexpForOneClientFromDictionary : IvyBindingFree, free, hash_del\n");
    /* on efface le binding */
    IvyBindingFree (msgSendDict->binding);
    /* on enlève l'entrée regexp de la table de hash */
    HASH_DEL (messSndByRegexp, msgSendDict);
    /* on efface la clef (regexp source) */
    free (msgSendDict->regexp_src);
    /* on libère la structure */
    free (msgSendDict);
  }

}


static void delAllRegexpsFromDictionary ()
{
  MsgSndDictPtr msgSendDict;
  RWIvyClientPtr  client;

  /* pour toutes les entrees du dictionnaire des regexps */
  for (msgSendDict=messSndByRegexp; msgSendDict ;
       msgSendDict= (MsgSndDictPtr) msgSendDict->hh.next) {
    /* on efface le binding */
    IvyBindingFree (msgSendDict->binding);
    /* pour chaque client abonne a cette regexp */
    IVY_LIST_EACH ( msgSendDict->clientList, client) {
      freeClient (client);
    }
    /* on enleve la liste de regexps */
    IVY_LIST_EMPTY(msgSendDict->clientList);
    /* on enleve le couple regexp -> valeur */
    HASH_DEL(messSndByRegexp, msgSendDict);
    /* on efface la clef (regexp source) */
    free (msgSendDict->regexp_src);
    /* on libère la structure */
    //    free (msgSendDict);
  }

#ifdef OPENMP
  regenerateRegPtrArrayCache ();
#endif
}









// HASH_ADD_KEYPTR  	 (hh_name, head, key_ptr, key_len, item_ptr)
// HASH_ADD_STR  	 (         head, keyfield_name,    item_ptr)

static void addRegexpToDictionary (const char* regexp, IvyClientPtr client)
{
  MsgSndDictPtr msgSendDict = NULL;
  RWIvyClientPtr  newClient = NULL;
  char *errorbuffer = IvyGetCurrentContext()->ivy_regexp_error_buffer;
  /* on cherche si une entrée existe deja pour cette regexp source */
  HASH_FIND_STR(messSndByRegexp, regexp, msgSendDict);
    /* l'entree n'existe pas dans le dictionnaire : on la cree */
  if (msgSendDict == NULL) {
    const char *errbuf;
    int erroffset;

    msgSendDict = (MsgSndDictPtr) malloc (sizeof (struct _msg_snd_dict));
    msgSendDict->regexp_src = strdup (regexp);

    msgSendDict->binding = IvyBindingCompile(regexp, & erroffset, & errbuf );
    if (msgSendDict->binding  == NULL ) {
			snprintf(errorbuffer, sizeof(IvyGetCurrentContext()->ivy_regexp_error_buffer), "Error compiling '%s', %s", regexp, errbuf);
      printf("%s\n", errorbuffer);
      MsgSendTo(client, Error, erroffset, errorbuffer );
    }

    msgSendDict->clientList = NULL;

    /* HASH_ADD_STR ne fonctionne que si la clef est un tableau de char, si c'est un pointeur
       if faut utiliser HASH_ADD_KEYPTR */
#ifdef DEBUG
    {// DEBUG
      int debugSize=0, nDebugSize=0;
      MsgSndDictPtr msd;
      for (msd=messSndByRegexp; msd ; msd= (MsgSndDictPtr) msd->hh.next) {
	debugSize++;
      }
      HASH_ADD_KEYPTR(hh, messSndByRegexp, msgSendDict->regexp_src, strlen (msgSendDict->regexp_src), msgSendDict);
      for (msd=messSndByRegexp; msd ; msd= (MsgSndDictPtr) msd->hh.next) {
	nDebugSize++;
      }
      if ((nDebugSize-debugSize) != 1) {
	printf ("DBG> Hash ERROR, adding %s let hashsize to pass from %d to %d\n", regexp, debugSize, nDebugSize);
      } else {
	printf ("DBG> adding %s[%d]\n", regexp, nDebugSize);
      }

    }// END DEBUG
#else
    HASH_ADD_KEYPTR(hh, messSndByRegexp, msgSendDict->regexp_src, strlen (msgSendDict->regexp_src), msgSendDict);
#endif

#ifdef OPENMP
    // On ne regenere le cache qu'après recpetion du endregexp, ça permet d'eviter
    // de regenerer inutilement le cache à chaqye nouvelle regexp initiale
    // par contre, après le end regexp, il faut regenerer le cache à chaque
    // nouvel abonnement
    if (client->endRegexpReceived == 1)
      addRegToPtrArrayCache (msgSendDict);
#endif
  }


  /* on ajoute le client à la liste des clients abonnés */
  IVY_LIST_ADD_START (msgSendDict->clientList, newClient);
  newClient->app_name = strdup (client->app_name);
  newClient->app_port = client->app_port;
  newClient->client = client->client;
  newClient->id = client->id;
  /* au niveau du champ liste de client du dictionnaire, on n'a pas besoin
     de la liste des regexps sources (qui n'est necessaire que pour
     la liste globale des clients) */
  newClient->srcRegList = NULL;
  IVY_LIST_ADD_END (msgSendDict->clientList, newClient);
}



static void changeRegexpInDictionary (const char* old_regexp, const char* new_regexp, IvyClientPtr client)
{
  //  printf ("DBG> ENTER changeRegexpInDictionary\n");
  if (old_regexp)
    delRegexpForOneClientFromDictionary (old_regexp, client);
  addRegexpToDictionary (new_regexp, client);
}




/* met a jour le dictionnaire et la liste globale */
static void delOneClient (const Client client)
{
  RWIvyClientPtr client_itr, next;
  MsgSndDictPtr msgSendDict, mnext=NULL;

  /* on cherche le client dans la liste globale des clients */
  IVY_LIST_EACH_SAFE(allClients, client_itr, next) {
    /* si on le trouve */
    if (client_itr->client == client) {

      /* pour chaque regexp source de ce client */
      while (client_itr->srcRegList != NULL) {
	int regexp_id = client_itr->srcRegList->id;
	/* on met a jour la liste des clients associee a la regexp source */
	if (!delRegexpForOneClient (client_itr, regexp_id)) {
	  GlobRegPtr regxpSrc = client_itr->srcRegList;
	  if (regxpSrc->str_regexp != NULL) {
	    free (regxpSrc->str_regexp);
	    regxpSrc->str_regexp = NULL;
	  }
	  IVY_LIST_REMOVE (client_itr->srcRegList, regxpSrc);
	}
	/* on libere la memoire associee a la regexp source */
	/* probablement deja fait ailleurs d'après valgrind */
	/*      if (regxpSrc->str_regexp != NULL) { */
	/* 	free (regxpSrc->str_regexp); */
	/* 	regxpSrc->str_regexp = NULL; */
	/*       } */
      }


      /* on libere la liste de regexp source */
      IVY_LIST_EMPTY (client_itr->srcRegList);
      /* on enleve l'entree correspondant a ce client dans la liste globale */
      IVY_LIST_REMOVE (allClients, client_itr);

    }
  }


  /* on cherche dans le dictionnaire des regexps, les regexps qui ont
     ce client dans leur liste de client associés et on vire les entrées*/


  // forme un peu compliquée pour faire un parcours de liste "securisé" car
  // on libère la mémoire de l'element courrant dans le corp de la boucle
  // ce qui oblige a recuperer le champ next avant de faire ce free
  for ( msgSendDict = messSndByRegexp ;
	(mnext = msgSendDict ? (MsgSndDictPtr) msgSendDict->hh.next
	                     :  msgSendDict ),msgSendDict ;
	msgSendDict = mnext ) {
    delOneClientFromDictionaryEntry (msgSendDict, client);
  }

#ifdef OPENMP
  regenerateRegPtrArrayCache ();
#endif
}


static char delRegexpForOneClient (IvyClientPtr client, int id)
{
  RWIvyClientPtr client_itr = NULL;
  GlobRegPtr   regxpSrc = NULL, next = NULL;
  char removed = 0;



  TRACE ("ENTER delRegexpForOneClient id=%d\n", id);
  //  printf ("DBG> ENTER delRegexpForOneClient id=%d\n", id);

  /* on enleve du dictionnaire */

  /* on enleve de la liste globale */
  /* recherche du client */
  IVY_LIST_ITER (allClients, client_itr,  client_itr->client != client->client );
  if (client_itr != NULL) {
    /* pour chaque regexp source de ce client */
    IVY_LIST_EACH_SAFE (client_itr->srcRegList, regxpSrc, next) {
      /* si on trouve notre regexp, on la supprime */
      if (regxpSrc->id == id) {
	removed = 1;
	if (regxpSrc->str_regexp != NULL) {
	  delRegexpForOneClientFromDictionary (regxpSrc->str_regexp, client);
	  free (regxpSrc->str_regexp);
	  regxpSrc->str_regexp = NULL;
	}
	TRACE ("DBG> IVY_LIST_REMOVE (%p, %p)\n", client_itr->srcRegList, regxpSrc);
	IVY_LIST_REMOVE (client_itr->srcRegList, regxpSrc);
      }
    }
  }
  return (removed);
}

static IvyBindEvent addOrChangeRegexp (const char* regexp, IvyClientPtr client)
{
  RWIvyClientPtr client_itr = NULL;
  GlobRegPtr regxpSrc = NULL;

  //  printf ("ENTER addOrChangeRegexp\n");
  /* Le changement de regexp est identifié par le couple client/id, même si
     le texte de regexp change et tombe donc dans une autre entrée hash. */
  IVY_LIST_ITER (allClients, client_itr, client_itr->client != client->client);
  if (client_itr != NULL) {
    IVY_LIST_EACH (client_itr->srcRegList, regxpSrc) {
      if (regxpSrc->id == client->id) {
	return changeRegexp (regxpSrc->str_regexp, regexp, client);
      }
    }
  }

  return addRegexp (regexp, client);
}


static IvyBindEvent addRegexp (const char* regexp, IvyClientPtr client)
{
  RWIvyClientPtr client_itr = NULL;
  GlobRegPtr   regxpSrc = NULL;


  //  printf ("ENTER addRegexp\n");

  /* on ajoute au dictionnaire */
  addRegexpToDictionary (regexp, client);

  /* on ajoute a la liste globale */
  /* recherche du client */
  IVY_LIST_ITER (allClients, client_itr,  client_itr->client != client->client );

  /* si le client n'existe pas, faut le creer */
  if (client_itr == NULL) {
/*     IVY_LIST_ADD_START (allClients, client_itr); */
/*     client_itr->app_name = strdup (client->app_name); */
/*     client_itr->app_port = client->app_port; */
/*     client_itr->client = client->client; */
/*     client_itr->srcRegList = NULL; */
/*     IVY_LIST_ADD_END (allClients, client_itr); */
    fprintf(stderr, "addRegexp ERROR\n");
  }

  /* on ajoute la regexp à la liste de regexps */
  IVY_LIST_ADD_START (client_itr->srcRegList, regxpSrc);
  regxpSrc->id = client->id;
  regxpSrc->str_regexp = strdup (regexp);
  IVY_LIST_ADD_END (client_itr->srcRegList, regxpSrc);
  return IvyAddBind;
}



static IvyBindEvent changeRegexp (const char* old_regexp, const char* new_regexp, IvyClientPtr client)
{
  IvyClientPtr client_itr = NULL;
  GlobRegPtr   regxpSrc = NULL, next = NULL;
  /* on change dans le dictionnaire */
  // printf ("ENTER changeRegexp\n");
  changeRegexpInDictionary (old_regexp, new_regexp, client);

  /* on change dans la liste globale */
  /* recherche du client */
  IVY_LIST_ITER (allClients, client_itr,  client_itr->client != client->client );
  if (client_itr != NULL) {
    /* pour chaque regexp source de ce client */
    IVY_LIST_EACH_SAFE (client_itr->srcRegList, regxpSrc, next) {
      /* si on trouve notre regexp, on la change */
      if (regxpSrc->id == client->id) {
	free (regxpSrc->str_regexp);
	regxpSrc->str_regexp = strdup (new_regexp);
      }
    }
  }
  return IvyChangeBind;
}

#ifdef OPENMP
static void regenerateRegPtrArrayCache ()
{
  int count=0;
  MsgSndDictPtr msgSendDict;
  ompDictCache.numPtr = 0;

  for (msgSendDict=messSndByRegexp; msgSendDict != NULL ;
       msgSendDict=msgSendDict->hh.next) {
    ompDictCache.numPtr++;
  }

  if (ompDictCache.numPtr >= ompDictCache.size) {
    ompDictCache.size = (ompDictCache.numPtr*2) + 128;
    ompDictCache.msgPtrArray = realloc (ompDictCache.msgPtrArray,
					sizeof (MsgSndDictPtr) * ompDictCache.size);
  }


  for (msgSendDict=messSndByRegexp; msgSendDict != NULL ;
       msgSendDict=msgSendDict->hh.next) {
    ompDictCache.msgPtrArray [count++] = msgSendDict;
  }
}


static void addRegToPtrArrayCache (MsgSndDictPtr newReg)
{
  int count=0;
  MsgSndDictPtr msgSendDict;
  ompDictCache.numPtr = 0;

  for (msgSendDict=messSndByRegexp; msgSendDict != NULL ;
       msgSendDict=msgSendDict->hh.next) {
    ompDictCache.numPtr++;
  }

  if (ompDictCache.numPtr >= ompDictCache.size) {
    ompDictCache.size = (ompDictCache.numPtr*2) + 128;
    ompDictCache.msgPtrArray = realloc (ompDictCache.msgPtrArray,
					sizeof (MsgSndDictPtr) * ompDictCache.size);
    for (msgSendDict=messSndByRegexp; msgSendDict != NULL ;
	 msgSendDict=msgSendDict->hh.next) {
      ompDictCache.msgPtrArray [count++] = msgSendDict;
    }
  } else {
    // on ajoute juste le nouveau pointeur
    ompDictCache.msgPtrArray [ompDictCache.numPtr-1] = newReg;
  }
}
#endif // OPENMP
