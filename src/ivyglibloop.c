/*
 * Ivy, C interface -- GLib main-context backend.
 * Copyright (C) 1997-2000 Centre d'Études de la Navigation Aérienne
 * See version.h for the copyright notice regarding this software.
 */
#include <glib.h>
#include "ivychannel.h"
#include "ivyloop.h"
#include "ivyglibloop.h"
#include "ivyglibtimer.h"

struct _channel {
  Channel next;
  IvyChannelState *owner;
  IVY_HANDLE fd;
  gpointer poll_tag;
  void *data;
  gboolean removed;
  gboolean writable;
  ChannelHandleDelete handle_delete;
  ChannelHandleRead handle_read;
  ChannelHandleWrite handle_write;
};

struct _control_event {
  struct _control_event *next;
  IvyControlCallback callback;
  void *data;
};

/* GLib holds a source reference throughout dispatch. The mutex only protects
 * backend metadata; socket and user callbacks always run without it. */
struct _ivy_channel_state {
  GSource source;
  GMainContext *context;
  GMutex mutex;
  GCond dispatch_done;
  Channel channels;
  struct _control_event *control_head;
  struct _control_event *control_tail;
  IvyTimerState *timers;
  gboolean initialized;
  gboolean stopped;
  gboolean destroying;
  gboolean dispatching;
  IvyHookPtr before, after;
  void *before_data, *after_data;
};

static IvyChannelState *default_state;
static gsize default_initialized;

static IvyChannelState *normalize(IvyChannelState *state)
{ return state ? state : IvyChannelGetDefaultState(); }

static gboolean pending_locked(IvyChannelState *state)
{
  Channel channel;
  if (state->destroying)
    return FALSE;
  if (state->control_head)
    return TRUE;
  if (!state->stopped)
    for (channel = state->channels; channel; channel = channel->next)
      if (channel->removed)
        return TRUE;
  return FALSE;
}

static gboolean prepare(GSource *source, gint *timeout)
{
  IvyChannelState *state = (IvyChannelState *)source;
  gboolean ready;
  *timeout = -1;
  g_mutex_lock(&state->mutex);
  ready = pending_locked(state);
  g_mutex_unlock(&state->mutex);
  return ready;
}

static gboolean check(GSource *source)
{
  IvyChannelState *state = (IvyChannelState *)source;
  Channel channel;
  gboolean ready;
  g_mutex_lock(&state->mutex);
  ready = pending_locked(state);
  if (!state->stopped && !state->destroying)
    for (channel = state->channels; !ready && channel; channel = channel->next)
      if (channel->poll_tag &&
          g_source_query_unix_fd(source, channel->poll_tag))
        ready = TRUE;
  g_mutex_unlock(&state->mutex);
  return ready;
}

static void delete_channels(Channel channels)
{
  while (channels) {
    Channel next = channels->next;
    if (channels->handle_delete)
      channels->handle_delete(channels->data);
    g_free(channels);
    channels = next;
  }
}

static void collect_removed(IvyChannelState *state)
{
  Channel *link, garbage = NULL;
  g_mutex_lock(&state->mutex);
  for (link = &state->channels; *link;) {
    Channel channel = *link;
    if (!channel->removed) {
      link = &channel->next;
      continue;
    }
    *link = channel->next;
    if (channel->poll_tag)
      g_source_remove_unix_fd((GSource *)state, channel->poll_tag);
    channel->next = garbage;
    garbage = channel;
  }
  g_mutex_unlock(&state->mutex);
  delete_channels(garbage);
}

static gboolean dispatch(GSource *source, GSourceFunc callback, gpointer data)
{
  IvyChannelState *state = (IvyChannelState *)source;
  Channel channel;
  (void)callback;
  (void)data;

  g_mutex_lock(&state->mutex);
  if (state->destroying) {
    g_mutex_unlock(&state->mutex);
    return G_SOURCE_REMOVE;
  }
  state->dispatching = TRUE;
  g_mutex_unlock(&state->mutex);
  collect_removed(state);

  for (;;) {
    struct _control_event *event;
    g_mutex_lock(&state->mutex);
    event = state->control_head;
    if (event) {
      state->control_head = event->next;
      if (!state->control_head)
        state->control_tail = NULL;
    }
    g_mutex_unlock(&state->mutex);
    if (!event)
      break;
    event->callback(event->data);
    g_free(event);
  }

  g_mutex_lock(&state->mutex);
  channel = state->channels;
  while (channel && !state->stopped && !state->destroying) {
    Channel next = channel->next;
    GIOCondition condition = channel->poll_tag ?
      g_source_query_unix_fd(source, channel->poll_tag) : 0;
    if (!channel->removed && (condition & (G_IO_IN | G_IO_HUP)) &&
        channel->handle_read) {
      g_mutex_unlock(&state->mutex);
      channel->handle_read(channel, channel->fd, channel->data);
      g_mutex_lock(&state->mutex);
    }
    if (state->destroying)
      break;
    if (!state->stopped && !channel->removed && channel->writable &&
        (condition & G_IO_OUT) && channel->handle_write) {
      g_mutex_unlock(&state->mutex);
      channel->handle_write(channel, channel->fd, channel->data);
      g_mutex_lock(&state->mutex);
    }
    if (state->destroying)
      break;
    /* Drain readable data before closing on HUP. Readers remove on EOF. */
    if ((condition & (G_IO_ERR | G_IO_NVAL)) ||
        ((condition & G_IO_HUP) && !channel->handle_read))
      channel->removed = TRUE;
    channel = next;
  }
  state->dispatching = FALSE;
  g_cond_broadcast(&state->dispatch_done);
  g_mutex_unlock(&state->mutex);
  return G_SOURCE_CONTINUE;
}

static void finalize(GSource *source)
{
  IvyChannelState *state = (IvyChannelState *)source;
  while (state->control_head) {
    struct _control_event *event = state->control_head;
    state->control_head = event->next;
    g_free(event);
  }
  g_main_context_unref(state->context);
  g_cond_clear(&state->dispatch_done);
  g_mutex_clear(&state->mutex);
}

static GSourceFuncs source_funcs = {
  .prepare = prepare, .check = check, .dispatch = dispatch, .finalize = finalize
};

static IvyChannelState *create_state(GMainContext *context)
{
  IvyChannelState *state = (IvyChannelState *)
    g_source_new(&source_funcs, sizeof(*state));
  g_mutex_init(&state->mutex);
  g_cond_init(&state->dispatch_done);
  state->context = g_main_context_ref(context);
  state->timers = IvyGlibTimerStateCreate(context);
  g_source_set_name((GSource *)state, "Ivy channels and control");
  return state;
}

IvyChannelState *IvyChannelStateCreate(void)
{
  GMainContext *context = g_main_context_ref_thread_default();
  IvyChannelState *state = create_state(context);
  g_main_context_unref(context);
  return state;
}

IvyChannelState *IvyChannelGetDefaultState(void)
{
  if (g_once_init_enter(&default_initialized)) {
    default_state = create_state(g_main_context_default());
    TimerStateDestroy(default_state->timers);
    default_state->timers = TimerGetDefaultState();
    g_once_init_leave(&default_initialized, 1);
  }
  return default_state;
}

void IvyChannelStateDestroy(IvyChannelState *state)
{
  Channel channels;
  if (!state || state == default_state)
    return;
  IvyChannelStopFor(state);
  g_mutex_lock(&state->mutex);
  state->destroying = TRUE;
  g_mutex_unlock(&state->mutex);
  g_source_destroy((GSource *)state);
  g_mutex_lock(&state->mutex);
  while (state->dispatching && !g_main_context_is_owner(state->context))
    g_cond_wait(&state->dispatch_done, &state->mutex);
  channels = state->channels;
  state->channels = NULL;
  g_mutex_unlock(&state->mutex);
  delete_channels(channels);
  TimerStateDestroy(state->timers);
  g_source_unref((GSource *)state);
}

IvyTimerState *IvyChannelGetTimerState(IvyChannelState *state)
{ return normalize(state)->timers; }

static GIOCondition channel_events(Channel channel)
{
  return (channel->handle_read ? G_IO_IN : 0) |
    (channel->writable ? G_IO_OUT : 0) | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
}

int IvyChannelInitFor(IvyChannelState *state)
{
  Channel channel;
  state = normalize(state);
  g_mutex_lock(&state->mutex);
  if (state->destroying) {
    g_mutex_unlock(&state->mutex);
    return -1;
  }
  state->stopped = FALSE;
  for (channel = state->channels; channel; channel = channel->next)
    if (!channel->removed && !channel->poll_tag)
      channel->poll_tag = g_source_add_unix_fd((GSource *)state,
        channel->fd, channel_events(channel));
  if (!state->initialized) {
    g_source_attach((GSource *)state, state->context);
    state->initialized = TRUE;
  }
  g_mutex_unlock(&state->mutex);
  IvyGlibTimerSetStopped(state->timers, FALSE);
  return 0;
}

void IvyChannelInit(void) { (void)IvyChannelInitFor(NULL); }
void IvyChannelWakeFor(IvyChannelState *state)
{ g_main_context_wakeup(normalize(state)->context); }
void IvyChannelWake(void) { IvyChannelWakeFor(NULL); }

int IvyChannelPostControlFor(IvyChannelState *state,
                            IvyControlCallback callback, void *data)
{
  struct _control_event *event;
  if (!callback)
    return -1;
  state = normalize(state);
  event = g_try_new0(struct _control_event, 1);
  if (!event)
    return -1;
  event->callback = callback;
  event->data = data;
  g_mutex_lock(&state->mutex);
  if (state->destroying) {
    g_mutex_unlock(&state->mutex);
    g_free(event);
    return -1;
  }
  if (!state->initialized) {
    g_source_attach((GSource *)state, state->context);
    state->initialized = TRUE;
  }
  if (state->control_tail)
    state->control_tail->next = event;
  else
    state->control_head = event;
  state->control_tail = event;
  g_mutex_unlock(&state->mutex);
  IvyChannelWakeFor(state);
  return 0;
}

int IvyChannelPostControl(IvyControlCallback callback, void *data)
{ return IvyChannelPostControlFor(NULL, callback, data); }

int IvyChannelLoopIsActiveFor(IvyChannelState *state)
{
  gboolean enabled;
  state = normalize(state);
  g_mutex_lock(&state->mutex);
  enabled = state->initialized && !state->stopped;
  g_mutex_unlock(&state->mutex);
  if (!enabled)
    return 0;
  /* Recognize application-owned loops, including while asleep in poll.
   * Ownership is released when an external loop exits, so no stale thread ID
   * or active flag survives a later stop/destroy on a different thread. */
  if (g_main_context_is_owner(state->context))
    return 1;
  if (!g_main_context_acquire(state->context))
    return 1;
  g_main_context_release(state->context);
  return 0;
}
int IvyChannelLoopIsActive(void) { return IvyChannelLoopIsActiveFor(NULL); }
int IvyChannelIsLoopThreadFor(IvyChannelState *state)
{ return g_main_context_is_owner(normalize(state)->context); }
int IvyChannelIsLoopThread(void) { return IvyChannelIsLoopThreadFor(NULL); }

Channel IvyChannelAddFor(IvyChannelState *state, IVY_HANDLE fd, void *data,
                        ChannelHandleDelete on_delete, ChannelHandleRead on_read,
                        ChannelHandleWrite on_write)
{
  Channel channel;
  if (fd < 0)
    return NULL;
  state = normalize(state);
  channel = g_try_new0(struct _channel, 1);
  if (!channel)
    return NULL;
  channel->owner = state;
  channel->fd = fd;
  channel->data = data;
  channel->handle_delete = on_delete;
  channel->handle_read = on_read;
  channel->handle_write = on_write;
  g_mutex_lock(&state->mutex);
  if (!state->stopped)
    channel->poll_tag = g_source_add_unix_fd((GSource *)state, fd,
                                           channel_events(channel));
  channel->next = state->channels;
  state->channels = channel;
  g_mutex_unlock(&state->mutex);
  IvyChannelWakeFor(state);
  return channel;
}
Channel IvyChannelAdd(IVY_HANDLE fd, void *data, ChannelHandleDelete on_delete,
                      ChannelHandleRead on_read, ChannelHandleWrite on_write)
{ return IvyChannelAddFor(NULL, fd, data, on_delete, on_read, on_write); }

void IvyChannelRemove(Channel channel)
{
  IvyChannelState *state;
  if (!channel)
    return;
  state = channel->owner;
  g_mutex_lock(&state->mutex);
  channel->removed = TRUE;
  g_mutex_unlock(&state->mutex);
  IvyChannelWakeFor(state);
}

static void set_writable(IvyChannelState *state, Channel channel, gboolean enabled)
{
  if (!channel || (state && state != channel->owner))
    return;
  state = channel->owner;
  g_mutex_lock(&state->mutex);
  if (!channel->removed) {
    channel->writable = enabled;
    if (channel->poll_tag)
      g_source_modify_unix_fd((GSource *)state, channel->poll_tag,
                             channel_events(channel));
  }
  g_mutex_unlock(&state->mutex);
  IvyChannelWakeFor(state);
}
void IvyChannelAddWritableEventFor(IvyChannelState *state, Channel channel)
{ set_writable(state, channel, TRUE); }
void IvyChannelAddWritableEvent(Channel channel)
{ set_writable(NULL, channel, TRUE); }
void IvyChannelClearWritableEventFor(IvyChannelState *state, Channel channel)
{ set_writable(state, channel, FALSE); }
void IvyChannelClearWritableEvent(Channel channel)
{ set_writable(NULL, channel, FALSE); }

void IvyChannelStopFor(IvyChannelState *state)
{
  Channel channel;
  state = normalize(state);
  g_mutex_lock(&state->mutex);
  state->stopped = TRUE;
  /* Remove stopped descriptors from poll to avoid spinning on readable data. */
  for (channel = state->channels; channel; channel = channel->next) {
    if (channel->poll_tag)
      g_source_remove_unix_fd((GSource *)state, channel->poll_tag);
    channel->poll_tag = NULL;
  }
  g_mutex_unlock(&state->mutex);
  IvyGlibTimerSetStopped(state->timers, TRUE);
  IvyChannelWakeFor(state);
}
void IvyChannelStop(void) { IvyChannelStopFor(NULL); }

static gboolean iteration(IvyChannelState *state, gboolean block)
{
  IvyHookPtr before, after;
  void *before_data, *after_data;
  gboolean stopped;
  g_mutex_lock(&state->mutex);
  stopped = state->stopped;
  before = state->before;
  before_data = state->before_data;
  after = state->after;
  after_data = state->after_data;
  g_mutex_unlock(&state->mutex);
  if (stopped)
    return FALSE;
  if (!before && !after) {
    g_main_context_iteration(state->context, block);
  } else if (g_main_context_acquire(state->context)) {
    GPollFD *fds = NULL;
    gint priority, timeout, count, capacity = 0;
    g_main_context_prepare(state->context, &priority);
    while ((count = g_main_context_query(state->context, priority, &timeout,
                                         fds, capacity)) > capacity) {
      capacity = count;
      fds = g_renew(GPollFD, fds, capacity);
    }
    if (!block)
      timeout = 0;
    /* Preserve the select backend's hook contract: reacquire the application's
     * lock after polling, before any Ivy or GLib callback is dispatched. */
    if (before)
      before(before_data);
    g_main_context_get_poll_func(state->context)(fds, count, timeout);
    if (after)
      after(after_data);
    if (g_main_context_check(state->context, priority, fds, count))
      g_main_context_dispatch(state->context);
    g_free(fds);
    g_main_context_release(state->context);
  }
  return TRUE;
}

void IvyMainLoopFor(IvyChannelState *state)
{
  state = normalize(state);
  if (IvyChannelInitFor(state) != 0)
    return;
  g_source_ref((GSource *)state);
  /* Distinct loop threads must use distinct thread-default GMainContexts. */
  if (g_main_context_acquire(state->context)) {
    while (iteration(state, TRUE))
      ;
    g_main_context_release(state->context);
  }
  g_source_unref((GSource *)state);
}
void IvyMainLoop(void) { IvyMainLoopFor(NULL); }

void IvyIdleFor(IvyChannelState *state)
{
  state = normalize(state);
  g_mutex_lock(&state->mutex);
  if (!state->initialized) {
    g_mutex_unlock(&state->mutex);
    if (IvyChannelInitFor(state) != 0)
      return;
  } else {
    g_mutex_unlock(&state->mutex);
  }
  g_source_ref((GSource *)state);
  iteration(state, FALSE);
  g_source_unref((GSource *)state);
}
void IvyIdle(void) { IvyIdleFor(NULL); }

void IvySetBeforeSelectHookFor(IvyChannelState *state, IvyHookPtr hook, void *data)
{
  state = normalize(state);
  g_mutex_lock(&state->mutex);
  state->before = hook;
  state->before_data = data;
  g_mutex_unlock(&state->mutex);
}
void IvySetBeforeSelectHook(IvyHookPtr hook, void *data)
{ IvySetBeforeSelectHookFor(NULL, hook, data); }
void IvySetAfterSelectHookFor(IvyChannelState *state, IvyHookPtr hook, void *data)
{
  state = normalize(state);
  g_mutex_lock(&state->mutex);
  state->after = hook;
  state->after_data = data;
  g_mutex_unlock(&state->mutex);
}
void IvySetAfterSelectHook(IvyHookPtr hook, void *data)
{ IvySetAfterSelectHookFor(NULL, hook, data); }
