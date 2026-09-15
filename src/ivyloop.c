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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
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
#include "ivy_loop_internal.h"
#include "timer.h"
#include "ivy.h"
#include "ivy_timer_internal.h"
#include "ivythread.h"

struct _channel {
  Channel next;
  IvyChannelState *owner;
  IVY_HANDLE fd;
  void *data;
  int tobedeleted;
  int writable_requested;
  ChannelHandleDelete handle_delete;
  ChannelHandleRead handle_read;
  ChannelHandleWrite handle_write;
};

struct _control_event {
  struct _control_event *next;
  IvyControlCallback callback;
  void *data;
};

struct _ivy_channel_state {
  Channel channels_list;
  int channel_initialized;
  fd_set open_fds;
  fd_set wrdy_fds;
  IVY_HANDLE highestFd;
  int MainLoop;
  IvyMutex control_mutex;
  int control_mutex_initialized;
  struct _control_event *control_head;
  struct _control_event *control_tail;
  IvyThreadId loop_thread;
  int loop_thread_set;
  int loop_active;
#ifdef WIN32
  SOCKET wakeup_socket[2];
#else
  int wakeup_pipe[2];
#endif
  IvyHookPtr BeforeSelect;
  IvyHookPtr AfterSelect;
  void *BeforeSelectData;
  void *AfterSelectData;
  IvyTimerState *timer_state;
  int owns_timer_state;
};

static IvyChannelState default_channel_state = {
  .MainLoop = 1,
#ifdef WIN32
  .wakeup_socket = {INVALID_SOCKET, INVALID_SOCKET},
#else
  .wakeup_pipe = {-1, -1},
#endif
};

#ifdef IVY_TESTING
static int ivy_testing_channel_init_fail_step;

void
IvyTestingChannelInitFailStep(int step)
{
  ivy_testing_channel_init_fail_step = step;
}

static int
IvyTestingChannelInitShouldFail(int step)
{
  return ivy_testing_channel_init_fail_step == step;
}
#else
#define IvyTestingChannelInitShouldFail(step) 0
#endif

#ifdef WIN32
WSADATA WsaData;
#endif

static IvyChannelState *
IvyChannelNormalizeState(IvyChannelState *state)
{
  return state ? state : &default_channel_state;
}

static int
IvyChannelIsValidSelectFd(IVY_HANDLE fd)
{
#ifdef WIN32
  return fd != INVALID_SOCKET;
#elif defined(FD_SETSIZE)
  if (fd < 0)
    return 0;
  return fd < FD_SETSIZE;
#else
  (void)fd;
  return 1;
#endif
}

IvyChannelState *
IvyChannelGetDefaultState(void)
{
  return &default_channel_state;
}

static void
IvyChannelStateInitFields(IvyChannelState *state)
{
  state->MainLoop = 1;
#ifdef WIN32
  state->wakeup_socket[0] = INVALID_SOCKET;
  state->wakeup_socket[1] = INVALID_SOCKET;
#else
  state->wakeup_pipe[0] = -1;
  state->wakeup_pipe[1] = -1;
#endif
}

IvyChannelState *
IvyChannelStateCreate(void)
{
  IvyChannelState *state = (IvyChannelState *)calloc(1, sizeof(*state));

  if (!state)
    return NULL;

  IvyChannelStateInitFields(state);
  state->timer_state = TimerStateCreate();
  if (!state->timer_state) {
    free(state);
    return NULL;
  }
  state->owns_timer_state = 1;
  return state;
}

IvyTimerState *
IvyChannelGetTimerState(IvyChannelState *state)
{
  state = IvyChannelNormalizeState(state);
  if (!state->timer_state)
    state->timer_state = TimerGetDefaultState();
  return state->timer_state;
}

static void
IvyWakeupClose(IvyChannelState *state)
{
#ifdef WIN32
  if (state->wakeup_socket[0] != INVALID_SOCKET)
    closesocket(state->wakeup_socket[0]);
  if (state->wakeup_socket[1] != INVALID_SOCKET)
    closesocket(state->wakeup_socket[1]);
  state->wakeup_socket[0] = INVALID_SOCKET;
  state->wakeup_socket[1] = INVALID_SOCKET;
#else
  if (state->wakeup_pipe[0] >= 0)
    close(state->wakeup_pipe[0]);
  if (state->wakeup_pipe[1] >= 0)
    close(state->wakeup_pipe[1]);
  state->wakeup_pipe[0] = -1;
  state->wakeup_pipe[1] = -1;
#endif
}

static void IvyChannelDrainControlFor(IvyChannelState *state);
static void IvyChannelDeleteFor(IvyChannelState *state, Channel channel);

void
IvyChannelStateDestroy(IvyChannelState *state)
{
  Channel channel;
  struct _control_event *event;

  if (!state || state == &default_channel_state)
    return;

  while (state->channels_list) {
    channel = state->channels_list;
    IvyChannelDeleteFor(state, channel);
  }

  while (state->control_head) {
    event = state->control_head;
    state->control_head = event->next;
    free(event);
  }
  state->control_tail = NULL;

  IvyWakeupClose(state);

  if (state->control_mutex_initialized)
    IvyMutexDestroy(&state->control_mutex);
  if (state->owns_timer_state)
    TimerStateDestroy(state->timer_state);
  free(state);
}

static int
IvyControlInit(IvyChannelState *state)
{
  state = IvyChannelNormalizeState(state);
  if (!state->control_mutex_initialized) {
    if (IvyMutexInit(&state->control_mutex) != 0)
      return -1;
    state->control_mutex_initialized = 1;
  }
  return 0;
}

#ifdef WIN32
static void
IvySetSocketNonBlocking(SOCKET socket)
{
  u_long mode = 1;
  (void)ioctlsocket(socket, FIONBIO, &mode);
}

static void
IvyCloseSocketIfValid(SOCKET *socket)
{
  if (*socket != INVALID_SOCKET) {
    closesocket(*socket);
    *socket = INVALID_SOCKET;
  }
}

static int
IvyWakeupInit(IvyChannelState *state)
{
  SOCKET listener = INVALID_SOCKET;
  SOCKET reader = INVALID_SOCKET;
  SOCKET writer = INVALID_SOCKET;
  struct sockaddr_in addr;
  int addr_len = sizeof(addr);
  int ok = 0;

  if (state->wakeup_socket[0] != INVALID_SOCKET)
    return 0;

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET)
    goto cleanup;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    goto cleanup;
  if (listen(listener, 1) == SOCKET_ERROR)
    goto cleanup;
  if (getsockname(listener, (struct sockaddr *)&addr, &addr_len) == SOCKET_ERROR)
    goto cleanup;

  writer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (writer == INVALID_SOCKET)
    goto cleanup;
  if (connect(writer, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    goto cleanup;

  reader = accept(listener, NULL, NULL);
  if (reader == INVALID_SOCKET)
    goto cleanup;

  IvySetSocketNonBlocking(reader);
  IvySetSocketNonBlocking(writer);
  state->wakeup_socket[0] = reader;
  state->wakeup_socket[1] = writer;
  reader = INVALID_SOCKET;
  writer = INVALID_SOCKET;
  ok = 1;

cleanup:
  IvyCloseSocketIfValid(&listener);
  IvyCloseSocketIfValid(&reader);
  IvyCloseSocketIfValid(&writer);
  return ok ? 0 : -1;
}

static int
IvyWakeupRegister(IvyChannelState *state)
{
  if (state->wakeup_socket[0] == INVALID_SOCKET)
    return -1;
  if (!IvyChannelIsValidSelectFd(state->wakeup_socket[0]))
    return -1;

  if (state->wakeup_socket[0] >= state->highestFd)
    state->highestFd = state->wakeup_socket[0] + 1;
  FD_SET(state->wakeup_socket[0], &state->open_fds);
  return 0;
}

static int
IvyWakeupIsReady(IvyChannelState *state, fd_set *current)
{
  return state->wakeup_socket[0] != INVALID_SOCKET &&
    FD_ISSET(state->wakeup_socket[0], current);
}

static void
IvyWakeupDrain(IvyChannelState *state)
{
  char buffer[64];

  if (state->wakeup_socket[0] == INVALID_SOCKET)
    return;

  for (;;) {
    int nb = recv(state->wakeup_socket[0], buffer, sizeof(buffer), 0);
    if (nb > 0)
      continue;
    if (nb == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
      continue;
    break;
  }
}
#else
static void
IvySetNonBlocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int
IvyWakeupInit(IvyChannelState *state)
{
  if (state->wakeup_pipe[0] >= 0)
    return 0;

  if (pipe(state->wakeup_pipe) < 0)
    return -1;

  IvySetNonBlocking(state->wakeup_pipe[0]);
  IvySetNonBlocking(state->wakeup_pipe[1]);
  return 0;
}

static int
IvyWakeupRegister(IvyChannelState *state)
{
  if (state->wakeup_pipe[0] < 0)
    return -1;
  if (!IvyChannelIsValidSelectFd(state->wakeup_pipe[0]))
    return -1;

  if (state->wakeup_pipe[0] >= state->highestFd)
    state->highestFd = state->wakeup_pipe[0] + 1;
  FD_SET(state->wakeup_pipe[0], &state->open_fds);
  return 0;
}

static int
IvyWakeupIsReady(IvyChannelState *state, fd_set *current)
{
  return state->wakeup_pipe[0] >= 0 && FD_ISSET(state->wakeup_pipe[0], current);
}

static void
IvyWakeupDrain(IvyChannelState *state)
{
  char buffer[64];

  if (state->wakeup_pipe[0] < 0)
    return;

  for (;;) {
    ssize_t nb = read(state->wakeup_pipe[0], buffer, sizeof(buffer));
    if (nb > 0)
      continue;
    if (nb < 0 && errno == EINTR)
      continue;
    break;
  }
}
#endif

void
IvyChannelWakeFor(IvyChannelState *state)
{
  state = IvyChannelNormalizeState(state);
#ifdef WIN32
  char wake = 'w';
  int sent;

  if (state->wakeup_socket[1] == INVALID_SOCKET)
    return;

  do {
    sent = send(state->wakeup_socket[1], &wake, 1, IVY_MSG_NOSIGNAL);
  } while (sent == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
#else
  char wake = 'w';
  ssize_t written;

  if (state->wakeup_pipe[1] < 0)
    return;

  do {
    written = write(state->wakeup_pipe[1], &wake, 1);
  } while (written < 0 && errno == EINTR);
#endif
}

void
IvyChannelWake(void)
{
  IvyChannelWakeFor(IvyChannelGetDefaultState());
}

static void
IvyChannelDrainControlFor(IvyChannelState *state)
{
  state = IvyChannelNormalizeState(state);
  for (;;) {
    struct _control_event *event;

    if (!state->control_mutex_initialized)
      return;

    IvyMutexLock(&state->control_mutex);
    event = state->control_head;
    if (event) {
      state->control_head = event->next;
      if (!state->control_head)
	state->control_tail = NULL;
    }
    IvyMutexUnlock(&state->control_mutex);

    if (!event)
      return;

    if (event->callback)
      (*event->callback)(event->data);
    free(event);
  }
}

int
IvyChannelPostControlFor(IvyChannelState *state, IvyControlCallback callback, void *data)
{
  struct _control_event *event;

  state = IvyChannelNormalizeState(state);
  if (!callback)
    return -1;
  if (IvyControlInit(state) != 0)
    return -1;

  event = (struct _control_event *)malloc(sizeof(*event));
  if (!event)
    return -1;
  event->next = NULL;
  event->callback = callback;
  event->data = data;

  IvyMutexLock(&state->control_mutex);
  if (state->control_tail)
    state->control_tail->next = event;
  else
    state->control_head = event;
  state->control_tail = event;
  IvyMutexUnlock(&state->control_mutex);

  IvyChannelWakeFor(state);
  return 0;
}

int
IvyChannelPostControl(IvyControlCallback callback, void *data)
{
  return IvyChannelPostControlFor(IvyChannelGetDefaultState(), callback, data);
}

static int
IvyChannelKeepRunning(void *data)
{
  IvyChannelState *state = data;
  int running;
  IvyMutexLock(&state->control_mutex);
  running = state->MainLoop;
  IvyMutexUnlock(&state->control_mutex);
  return running;
}

static int
IvyChannelAcquireLoop(IvyChannelState *state)
{
  int status = IVY_OK;
  IvyMutexLock(&state->control_mutex);
  if (state->loop_active)
    status = IVY_ESTATE;
  else {
    state->loop_thread = IvyThreadCurrent();
    state->loop_thread_set = 1;
    state->loop_active = 1;
  }
  IvyMutexUnlock(&state->control_mutex);
  return status;
}

/* Drain accepted controls and clear ownership under the queue mutex so a
 * control already enqueued at exit cannot be missed by the final drain. */
static void
IvyChannelReleaseLoop(IvyChannelState *state)
{
  for (;;) {
    IvyChannelDrainControlFor(state);
    IvyMutexLock(&state->control_mutex);
    if (!state->control_head) {
      state->loop_active = 0;
      state->loop_thread_set = 0;
      IvyMutexUnlock(&state->control_mutex);
      return;
    }
    IvyMutexUnlock(&state->control_mutex);
  }
}

int
IvyChannelLoopIsActiveFor(IvyChannelState *state)
{
  int active = 0;

  state = IvyChannelNormalizeState(state);
  if (!state->control_mutex_initialized)
    return 0;

  IvyMutexLock(&state->control_mutex);
  active = state->loop_active;
  IvyMutexUnlock(&state->control_mutex);
  return active;
}

int
IvyChannelLoopIsActive(void)
{
  return IvyChannelLoopIsActiveFor(IvyChannelGetDefaultState());
}

int
IvyChannelIsLoopThreadFor(IvyChannelState *state)
{
  int is_loop_thread = 0;

  state = IvyChannelNormalizeState(state);
  if (!state->control_mutex_initialized)
    return 0;

  IvyMutexLock(&state->control_mutex);
  if (state->loop_thread_set)
    is_loop_thread = IvyThreadEqual(state->loop_thread, IvyThreadCurrent());
  IvyMutexUnlock(&state->control_mutex);
  return is_loop_thread;
}

int
IvyChannelIsLoopThread(void)
{
  return IvyChannelIsLoopThreadFor(IvyChannelGetDefaultState());
}

void
IvyChannelRemove (Channel channel)
{
  if (channel)
    channel->tobedeleted = 1;
}

static void
IvyChannelDeleteFor(IvyChannelState *state, Channel channel)
{
  state = IvyChannelNormalizeState(state ? state : (channel ? channel->owner : NULL));
  if (!channel)
    return;

  if (channel->handle_delete)
    (*channel->handle_delete) (channel->data);

  FD_CLR (channel->fd, &state->open_fds);
  FD_CLR (channel->fd, &state->wrdy_fds);
  IVY_LIST_REMOVE (state->channels_list, channel);
}

static void
ChannelDefferedDeleteFor(IvyChannelState *state)
{
  Channel channel, next;
  state = IvyChannelNormalizeState(state);
  IVY_LIST_EACH_SAFE (state->channels_list, channel,next)	{
    if (channel->tobedeleted ) {
      IvyChannelDeleteFor (state, channel);
    }
  }
}

Channel IvyChannelAddFor (IvyChannelState *state, IVY_HANDLE fd, void *data,
		       ChannelHandleDelete handle_delete,
		       ChannelHandleRead handle_read,
		       ChannelHandleWrite handle_write
		       )
{
  Channel channel;

  state = IvyChannelNormalizeState(state);
  if (!IvyChannelIsValidSelectFd(fd))
    return NULL;

  IVY_LIST_ADD_START (state->channels_list, channel)
    channel->owner = state;
    channel->fd = fd;
  channel->tobedeleted = 0;
  channel->handle_delete = handle_delete;
  channel->handle_read = handle_read;
  channel->handle_write = handle_write;
  channel->data = data;
  if (channel->fd >= state->highestFd)
    state->highestFd = channel->fd+1 ;

  IVY_LIST_ADD_END (state->channels_list, channel)

    FD_SET (channel->fd, &state->open_fds);

  return channel;
}

Channel IvyChannelAdd (IVY_HANDLE fd, void *data,
		       ChannelHandleDelete handle_delete,
		       ChannelHandleRead handle_read,
		       ChannelHandleWrite handle_write
		       )
{
  return IvyChannelAddFor(IvyChannelGetDefaultState(), fd, data,
			  handle_delete, handle_read, handle_write);
}

static void IvyChannelSetWritable(IvyChannelState *state, Channel channel, int enabled)
{
  if (!channel) return;
  state = IvyChannelNormalizeState(state ? state : channel->owner);
  if (IvyControlInit(state) != 0) return;
  IvyMutexLock(&state->control_mutex);
  channel->writable_requested = enabled;
  IvyMutexUnlock(&state->control_mutex);
  IvyChannelWakeFor(state);
}

/* Only the loop mutates fd_sets. No queued callback retains a channel pointer
 * after deletion, and requesting writable interest cannot fail allocation. */
static void IvyChannelApplyWritable(IvyChannelState *state)
{
  Channel channel;
  IvyMutexLock(&state->control_mutex);
  FD_ZERO(&state->wrdy_fds);
  IVY_LIST_EACH(state->channels_list, channel) {
    if (!channel->tobedeleted && channel->writable_requested)
      FD_SET(channel->fd, &state->wrdy_fds);
  }
  IvyMutexUnlock(&state->control_mutex);
}

void IvyChannelAddWritableEventFor(IvyChannelState *state, Channel channel)
{ IvyChannelSetWritable(state, channel, 1); }
void IvyChannelAddWritableEvent(Channel channel)
{ IvyChannelSetWritable(NULL, channel, 1); }
void IvyChannelClearWritableEventFor(IvyChannelState *state, Channel channel)
{ IvyChannelSetWritable(state, channel, 0); }
void IvyChannelClearWritableEvent(Channel channel)
{ IvyChannelSetWritable(NULL, channel, 0); }

static void
IvyChannelHandleWrite (IvyChannelState *state, fd_set *current)
{
  Channel channel, next;

  IVY_LIST_EACH_SAFE (state->channels_list, channel, next) {
    if (!IvyChannelKeepRunning(state))
      break;
    if (!channel->tobedeleted && FD_ISSET (channel->fd, current)) {
      if (channel->handle_write)
	(*channel->handle_write)(channel,channel->fd,channel->data);
    }
  }
}

static void
IvyChannelHandleRead (IvyChannelState *state, fd_set *current)
{
  Channel channel, next;

  IVY_LIST_EACH_SAFE (state->channels_list, channel, next) {
    if (!IvyChannelKeepRunning(state))
      break;
    if (!channel->tobedeleted && FD_ISSET (channel->fd, current)) {
      if (channel->handle_read)
	(*channel->handle_read)(channel,channel->fd,channel->data);
    }
  }
}

static void
IvyChannelHandleExcpt (IvyChannelState *state, fd_set *current)
{
  Channel channel,next;
  IVY_LIST_EACH_SAFE (state->channels_list, channel, next) {
    if (!IvyChannelKeepRunning(state))
      break;
    if (!channel->tobedeleted && FD_ISSET (channel->fd, current)) {
      if (channel->handle_delete)
	(*channel->handle_delete)(channel->data);
      /*			IvyChannelClose (channel); */
    }
  }
}

int IvyChannelInitFor (IvyChannelState *state)
{
  state = IvyChannelNormalizeState(state);
#ifdef WIN32
  int error;
#else
  /* pour eviter les plantages quand les autres applis font core-dump */
  /* signal (SIGPIPE, SIG_IGN); removed: managed locally by sockets */
#endif
  if (IvyTestingChannelInitShouldFail(IVY_TEST_CHANNEL_INIT_FAIL_CONTROL))
    return -1;
  if (IvyControlInit(state) != 0)
    return -1;

  /* Explicit initialization also prepares the reused legacy default state.
   * Checked run entry skips initialization once the state is initialized. */
  IvyMutexLock(&state->control_mutex);
  state->MainLoop = 1;
  IvyMutexUnlock(&state->control_mutex);

  if (state->channel_initialized) return 0;

#ifdef WIN32
  error = WSAStartup (0x0101, &WsaData);
  if (error != 0)
    return -1;
#endif

  FD_ZERO (&state->open_fds);
  FD_ZERO (&state->wrdy_fds);
  state->highestFd = 0;
  (void)IvyChannelGetTimerState(state);

  if (IvyTestingChannelInitShouldFail(IVY_TEST_CHANNEL_INIT_FAIL_WAKEUP))
    return -1;
  if (IvyWakeupInit(state) != 0)
    return -1;
  if (IvyWakeupRegister(state) != 0) {
    IvyWakeupClose(state);
    return -1;
  }
  state->channel_initialized = 1;
  return 0;
}

void IvyChannelInit (void)
{
  (void)IvyChannelInitFor(IvyChannelGetDefaultState());
}

void IvyChannelStopFor (IvyChannelState *state)
{
  state = IvyChannelNormalizeState(state);
  if (IvyControlInit(state) != 0)
    return;
  IvyMutexLock(&state->control_mutex);
  state->MainLoop = 0;
  IvyMutexUnlock(&state->control_mutex);
  IvyChannelWakeFor(state);
}

void IvyChannelStop (void)
{
  IvyChannelStopFor(IvyChannelGetDefaultState());
}

int IvyLoopAcquireFor(IvyChannelState *state)
{
  state = IvyChannelNormalizeState(state);
  if (!state->channel_initialized && IvyChannelInitFor(state) != 0)
    return IVY_EIO;
  return IvyChannelAcquireLoop(state);
}

void IvyLoopReleaseFor(IvyChannelState *state)
{
  IvyChannelReleaseLoop(IvyChannelNormalizeState(state));
}

int IvyLoopRunOwnedFor(IvyChannelState *state)
{
  fd_set rdset, exset, wrset;
  int ready, error, status = IVY_OK;
  state = IvyChannelNormalizeState(state);
  while (IvyChannelKeepRunning(state)) {
    ChannelDefferedDeleteFor(state);
    IvyChannelDrainControlFor(state);
    if (!IvyChannelKeepRunning(state))
      break;

    if (state->BeforeSelect)
      (*state->BeforeSelect)(state->BeforeSelectData);
    IvyChannelApplyWritable(state);
    rdset = state->open_fds;
    wrset = state->wrdy_fds;
    exset = state->open_fds;
    ready = select(state->highestFd, &rdset, &wrset, &exset,
                   TimerGetSmallestTimeoutFor(IvyChannelGetTimerState(state)));
#ifdef WIN32
    error = ready < 0 ? WSAGetLastError() : 0;
#else
    error = ready < 0 ? errno : 0;
#endif
    if (state->AfterSelect)
      (*state->AfterSelect)(state->AfterSelectData);
    if (ready < 0) {
#ifdef WIN32
      if (error == WSAEINTR)
#else
      if (error == EINTR)
#endif
        continue;
      status = IVY_EIO;
      break;
    }
    if (ready > 0 && IvyWakeupIsReady(state, &rdset))
      IvyWakeupDrain(state);
    IvyChannelDrainControlFor(state);
    if (!IvyChannelKeepRunning(state))
      break;
    IvyTimerScanWhileFor(IvyChannelGetTimerState(state), IvyChannelKeepRunning, state);
    if (ready > 0 && IvyChannelKeepRunning(state)) {
      IvyChannelHandleExcpt(state, &exset);
      IvyChannelHandleRead(state, &rdset);
      IvyChannelHandleWrite(state, &wrset);
    }
  }
  return status;
}

int IvyMainLoopRunFor(IvyChannelState *state)
{
  int status = IvyLoopAcquireFor(state);
  if (status != IVY_OK)
    return status;
  status = IvyLoopRunOwnedFor(state);
  IvyLoopReleaseFor(state);
  return status;
}

void IvyMainLoopFor(IvyChannelState *state)
{
  (void)IvyMainLoopRunFor(state);
}

void IvyMainLoop(void)
{
  IvyMainLoopFor(IvyChannelGetDefaultState());
}

void IvyIdleFor(IvyChannelState *state)
{
  fd_set rdset, exset, wrset;
  int ready;
  struct timeval timeout = {0,0};

  state = IvyChannelNormalizeState(state);
  if (IvyChannelInitFor(state) != 0)
    return;
  ChannelDefferedDeleteFor(state);
  IvyChannelDrainControlFor(state);
  IvyChannelApplyWritable(state);
  rdset = state->open_fds;
  wrset = state->wrdy_fds;
  exset = state->open_fds;
  ready = select(state->highestFd, &rdset, &wrset,  &exset, &timeout);
  if (ready < 0 && (errno != EINTR)) {
    fprintf (stderr, "select error %d\n",errno);
    perror("select");
    return;
  }
  if (ready > 0) {
    if (IvyWakeupIsReady(state, &rdset)) {
      IvyWakeupDrain(state);
    }
    IvyChannelDrainControlFor(state);
  }
  if (ready > 0) {
    IvyChannelHandleExcpt(state, &exset);
    IvyChannelHandleRead(state, &rdset);
    IvyChannelHandleWrite(state, &wrset);
  }
}

void IvyIdle()
{
  IvyIdleFor(IvyChannelGetDefaultState());
}



void IvySetBeforeSelectHookFor(IvyChannelState *state, IvyHookPtr before, void *data )
{
  state = IvyChannelNormalizeState(state);
  state->BeforeSelect = before;
  state->BeforeSelectData = data;
}

void IvySetBeforeSelectHook(IvyHookPtr before, void *data )
{
  IvySetBeforeSelectHookFor(IvyChannelGetDefaultState(), before, data);
}

void IvySetAfterSelectHookFor(IvyChannelState *state, IvyHookPtr after, void *data )
{
  state = IvyChannelNormalizeState(state);
  state->AfterSelect = after;
  state->AfterSelectData = data;
}

void IvySetAfterSelectHook(IvyHookPtr after, void *data )
{
  IvySetAfterSelectHookFor(IvyChannelGetDefaultState(), after, data);
}
