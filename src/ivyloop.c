/*
 *	Ivy, C interface
 *
 *	Copyright (C) 1997-2000
 *	Centre d'Études de la Navigation Aérienne
 *
 * 	Main loop based on select
 *
 *	Authors: François-Régis Colin <fcolin@cena.dgac.fr>
 *		 Stéphane Chatty <chatty@cena.dgac.fr>
 *
 *	$Id: ivyloop.c 3460 2011-01-24 13:39:15Z bustico $
 * 
 *	Please refer to file version.h for the
 *	copyright notice regarding this software
 */

#ifdef WIN32
#include <windows.h>
#endif
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifndef WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#endif

#include "list.h"
#include "ivychannel.h"
#include "ivyloop.h"
#include "timer.h"
#include "ivythread.h"

struct _channel {
  Channel next;
  IVY_HANDLE fd;
  void *data;
  int tobedeleted;
  ChannelHandleDelete handle_delete;
  ChannelHandleRead handle_read;
  ChannelHandleWrite handle_write;
};

static Channel channels_list = NULL;

static int channel_initialized = 0;

static fd_set open_fds;
static fd_set wrdy_fds;
static IVY_HANDLE highestFd=0;

static int MainLoop = 1;

struct _control_event {
  struct _control_event *next;
  IvyControlCallback callback;
  void *data;
};

static IvyMutex control_mutex;
static int control_mutex_initialized = 0;
static struct _control_event *control_head = NULL;
static struct _control_event *control_tail = NULL;
static IvyThreadId loop_thread;
static int loop_thread_set = 0;
static int loop_active = 0;

#ifndef WIN32
static int wakeup_pipe[2] = {-1, -1};
#endif

/* Hook callback & data */
static IvyHookPtr BeforeSelect = NULL;
static IvyHookPtr AfterSelect = NULL;

static void *BeforeSelectData = NULL;
static void *AfterSelectData = NULL;

#ifdef WIN32
WSADATA WsaData;
#endif

static int
IvyControlInit(void)
{
  if (!control_mutex_initialized) {
    if (IvyMutexInit(&control_mutex) != 0)
      return -1;
    control_mutex_initialized = 1;
  }
  return 0;
}

#ifndef WIN32
static void
IvySetNonBlocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int
IvyWakeupInit(void)
{
  if (wakeup_pipe[0] >= 0)
    return 0;

  if (pipe(wakeup_pipe) < 0)
    return -1;

  IvySetNonBlocking(wakeup_pipe[0]);
  IvySetNonBlocking(wakeup_pipe[1]);
  return 0;
}

static void
IvyWakeupRegister(void)
{
  if (wakeup_pipe[0] < 0)
    return;

  if (wakeup_pipe[0] >= highestFd)
    highestFd = wakeup_pipe[0] + 1;
  FD_SET(wakeup_pipe[0], &open_fds);
}

static void
IvyWakeupDrain(void)
{
  char buffer[64];

  if (wakeup_pipe[0] < 0)
    return;

  for (;;) {
    ssize_t nb = read(wakeup_pipe[0], buffer, sizeof(buffer));
    if (nb > 0)
      continue;
    if (nb < 0 && errno == EINTR)
      continue;
    break;
  }
}
#endif

void
IvyChannelWake(void)
{
#ifndef WIN32
  char wake = 'w';
  ssize_t written;

  if (wakeup_pipe[1] < 0)
    return;

  do {
    written = write(wakeup_pipe[1], &wake, 1);
  } while (written < 0 && errno == EINTR);
#endif
}

static void
IvyChannelDrainControl(void)
{
  for (;;) {
    struct _control_event *event;

    if (!control_mutex_initialized)
      return;

    IvyMutexLock(&control_mutex);
    event = control_head;
    if (event) {
      control_head = event->next;
      if (!control_head)
	control_tail = NULL;
    }
    IvyMutexUnlock(&control_mutex);

    if (!event)
      return;

    if (event->callback)
      (*event->callback)(event->data);
    free(event);
  }
}

int
IvyChannelPostControl(IvyControlCallback callback, void *data)
{
  struct _control_event *event;

  if (!callback)
    return -1;
  if (IvyControlInit() != 0)
    return -1;

  event = (struct _control_event *)malloc(sizeof(*event));
  if (!event)
    return -1;
  event->next = NULL;
  event->callback = callback;
  event->data = data;

  IvyMutexLock(&control_mutex);
  if (control_tail)
    control_tail->next = event;
  else
    control_head = event;
  control_tail = event;
  IvyMutexUnlock(&control_mutex);

  IvyChannelWake();
  return 0;
}

static void
IvyChannelSetLoopActive(int active)
{
  if (IvyControlInit() != 0)
    return;

  IvyMutexLock(&control_mutex);
  if (active) {
    loop_thread = IvyThreadCurrent();
    loop_thread_set = 1;
    loop_active = 1;
  } else {
    loop_active = 0;
  }
  IvyMutexUnlock(&control_mutex);
}

int
IvyChannelLoopIsActive(void)
{
  int active = 0;

  if (!control_mutex_initialized)
    return 0;

  IvyMutexLock(&control_mutex);
  active = loop_active;
  IvyMutexUnlock(&control_mutex);
  return active;
}

int
IvyChannelIsLoopThread(void)
{
  int is_loop_thread = 0;

  if (!control_mutex_initialized)
    return 0;

  IvyMutexLock(&control_mutex);
  if (loop_thread_set)
    is_loop_thread = IvyThreadEqual(loop_thread, IvyThreadCurrent());
  IvyMutexUnlock(&control_mutex);
  return is_loop_thread;
}

void
IvyChannelRemove (Channel channel)
{
  channel->tobedeleted = 1;
}

static void
IvyChannelDelete (Channel channel)
{
  if (channel->handle_delete)
    (*channel->handle_delete) (channel->data);

  FD_CLR (channel->fd, &open_fds);
  FD_CLR (channel->fd, &wrdy_fds);
  IVY_LIST_REMOVE (channels_list, channel);
}

static void
ChannelDefferedDelete ()
{
  Channel channel, next;
  IVY_LIST_EACH_SAFE (channels_list, channel,next)	{
    if (channel->tobedeleted ) {
      IvyChannelDelete (channel);
    }
  }
}

Channel IvyChannelAdd (IVY_HANDLE fd, void *data, 
		       ChannelHandleDelete handle_delete,
		       ChannelHandleRead handle_read,
		       ChannelHandleWrite handle_write
		       )						
{
  Channel channel;


  IVY_LIST_ADD_START (channels_list, channel)
    channel->fd = fd;
  channel->tobedeleted = 0;
  channel->handle_delete = handle_delete;
  channel->handle_read = handle_read;
  channel->handle_write = handle_write;
  channel->data = data;
  if (channel->fd >= highestFd)  
    highestFd = channel->fd+1 ;

  IVY_LIST_ADD_END (channels_list, channel)
    
    FD_SET (channel->fd, &open_fds);

  return channel;
}

static void IvyChannelAddWritableEventDirect(Channel channel)
{
  if (channel->fd >= highestFd)  
    highestFd = channel->fd+1 ;

  FD_SET (channel->fd, &wrdy_fds);
}

static void IvyChannelClearWritableEventDirect(Channel channel)
{
  FD_CLR (channel->fd, &wrdy_fds);
}

static void IvyChannelAddWritableEventControl(void *data)
{
  IvyChannelAddWritableEventDirect((Channel)data);
}

static void IvyChannelClearWritableEventControl(void *data)
{
  IvyChannelClearWritableEventDirect((Channel)data);
}

void IvyChannelAddWritableEvent(Channel channel)
{
  if (!channel)
    return;

  if (IvyChannelLoopIsActive() && !IvyChannelIsLoopThread()) {
    if (IvyChannelPostControl(IvyChannelAddWritableEventControl, channel) == 0)
      return;
  }

  IvyChannelAddWritableEventDirect(channel);
  IvyChannelWake();
}

void IvyChannelClearWritableEvent(Channel channel)
{
  if (!channel)
    return;

  if (IvyChannelLoopIsActive() && !IvyChannelIsLoopThread()) {
    if (IvyChannelPostControl(IvyChannelClearWritableEventControl, channel) == 0)
      return;
  }

  IvyChannelClearWritableEventDirect(channel);
  IvyChannelWake();
}

static void
IvyChannelHandleWrite (fd_set *current)
{
  Channel channel, next;
	
  IVY_LIST_EACH_SAFE (channels_list, channel, next) {
    if (FD_ISSET (channel->fd, current)) {
      (*channel->handle_write)(channel,channel->fd,channel->data);
    }
  }
}

static void
IvyChannelHandleRead (fd_set *current)
{
  Channel channel, next;
	
  IVY_LIST_EACH_SAFE (channels_list, channel, next) {
    if (FD_ISSET (channel->fd, current)) {
      (*channel->handle_read)(channel,channel->fd,channel->data);
    }
  }
}

static void
IvyChannelHandleExcpt (fd_set *current)
{
  Channel channel,next;
  IVY_LIST_EACH_SAFE (channels_list, channel, next) {
    if (FD_ISSET (channel->fd, current)) {
      if (channel->handle_delete)
	(*channel->handle_delete)(channel->data);
      /*			IvyChannelClose (channel); */
    }
  }
}

void IvyChannelInit (void)
{
#ifdef WIN32
  int error;
#else 
  /* pour eviter les plantages quand les autres applis font core-dump */
  signal (SIGPIPE, SIG_IGN);
#endif
  MainLoop = 1;
  if (IvyControlInit() != 0) {
    fprintf(stderr, "IvyChannelInit control mutex init failed\n");
    exit(0);
  }

  if (channel_initialized) return;

  FD_ZERO (&open_fds);
  FD_ZERO (&wrdy_fds);

#ifndef WIN32
  if (IvyWakeupInit() != 0) {
    perror("IvyChannelInit wakeup pipe");
    exit(0);
  }
  IvyWakeupRegister();
#endif

#ifdef WIN32
  error = WSAStartup (0x0101, &WsaData);
  if (error == SOCKET_ERROR) {
    printf ("WSAStartup failed.\n");
  }
#endif
  channel_initialized = 1;
}

void IvyChannelStop (void)
{
  MainLoop = 0;
  IvyChannelWake();
}

void IvyMainLoop(void)
{

  fd_set rdset, exset, wrset;
  int ready;

  IvyChannelSetLoopActive(1);
  while (MainLoop) {
		
    ChannelDefferedDelete();
    IvyChannelDrainControl();
    if (!MainLoop)
      break;
	   	
    if (BeforeSelect)
      (*BeforeSelect)(BeforeSelectData);
    rdset = open_fds;
    wrset = wrdy_fds;
    exset = open_fds;
    ready = select(highestFd, &rdset, &wrset,  &exset, 
		   TimerGetSmallestTimeout());
		
    if (AfterSelect) 
      (*AfterSelect)(AfterSelectData);
		
    if (ready < 0 && (errno != EINTR)) {
      fprintf (stderr, "select error %d\n",errno);
      perror("select");
      IvyChannelSetLoopActive(0);
      return;
    }
    if (ready > 0) {
#ifndef WIN32
      if (wakeup_pipe[0] >= 0 && FD_ISSET(wakeup_pipe[0], &rdset)) {
	IvyWakeupDrain();
      }
#endif
      IvyChannelDrainControl();
      if (!MainLoop)
	break;
    }
    TimerScan(); /* should be spliited in two part ( next timeout & callbacks */
    if (ready > 0) {
      IvyChannelHandleExcpt(&exset);
      IvyChannelHandleRead(&rdset);
      IvyChannelHandleWrite(&wrset);
    }
  }
  IvyChannelDrainControl();
  IvyChannelSetLoopActive(0);
}

void IvyIdle()
{
  fd_set rdset, exset, wrset;
  int ready;
  struct timeval timeout = {0,0}; 

	
  ChannelDefferedDelete();
  IvyChannelDrainControl();
  rdset = open_fds;
  wrset = wrdy_fds;
  exset = open_fds;
  ready = select(highestFd, &rdset, &wrset,  &exset, &timeout);
  if (ready < 0 && (errno != EINTR)) {
    fprintf (stderr, "select error %d\n",errno);
    perror("select");
    return;
  }
  if (ready > 0) {
#ifndef WIN32
    if (wakeup_pipe[0] >= 0 && FD_ISSET(wakeup_pipe[0], &rdset)) {
      IvyWakeupDrain();
    }
#endif
    IvyChannelDrainControl();
  }
  if (ready > 0) {
    IvyChannelHandleExcpt(&exset);
    IvyChannelHandleRead(&rdset);
    IvyChannelHandleWrite(&wrset);
  }
}



void IvySetBeforeSelectHook(IvyHookPtr before, void *data )
{
  BeforeSelect = before;
  BeforeSelectData = data;
}
void IvySetAfterSelectHook(IvyHookPtr after, void *data )
{
  AfterSelect = after;
  AfterSelectData = data;
}
