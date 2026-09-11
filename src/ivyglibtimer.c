/* GLib timer sources for Ivy. See version.h for the copyright notice. */
#include <glib.h>
#include <sys/time.h>
#include "ivyglibtimer.h"

struct _timer {
  TimerId next;
  IvyTimerState *owner;
  int repeat;
  gint64 period;
  gint64 when;
  TimerCb callback;
  void *data;
  gboolean removed;
};

struct _timer_state {
  GSource source;
  GMainContext *context;
  GMutex mutex;
  GCond scan_done;
  TimerId timers;
  gboolean stopped;
  gboolean scanning;
};

static IvyTimerState *default_state;
static gsize default_initialized;

static IvyTimerState *normalize(IvyTimerState *state)
{ return state ? state : TimerGetDefaultState(); }

/* Called with the timer lock held. Removed entries also need a dispatch so
 * cancellation releases storage even if no live timer remains. */
static gint64 deadline(IvyTimerState *state)
{
  TimerId timer;
  gint64 next = -1;
  if (state->stopped)
    return next;
  for (timer = state->timers; timer; timer = timer->next) {
    if (timer->removed)
      return 0;
    if (next < 0 || timer->when < next)
      next = timer->when;
  }
  return next;
}

static gboolean prepare(GSource *source, gint *timeout)
{
  IvyTimerState *state = (IvyTimerState *)source;
  gint64 next, remaining;
  g_mutex_lock(&state->mutex);
  next = deadline(state);
  g_mutex_unlock(&state->mutex);
  remaining = next < 0 ? -1 : MAX((gint64)0, next - g_get_monotonic_time());
  *timeout = remaining < 0 ? -1 : (gint)MIN((remaining + 999) / 1000, G_MAXINT);
  return remaining == 0;
}

static gboolean check(GSource *source)
{
  gint timeout;
  return prepare(source, &timeout);
}

void TimerScanFor(IvyTimerState *state)
{
  TimerId timer, *link;
  gint64 now = g_get_monotonic_time();
  state = normalize(state);
  g_source_ref((GSource *)state);
  g_mutex_lock(&state->mutex);
  if (state->scanning || state->stopped)
    goto done;
  state->scanning = TRUE;
  /* Defer removal until the scan finishes: callbacks may cancel themselves
   * or another due timer without invalidating this traversal. */
  for (timer = state->timers; timer && !state->stopped; timer = timer->next) {
    if (!timer->removed && timer->when <= now) {
      unsigned long delta = (unsigned long)((now - timer->when) / 1000);
      timer->when = now + timer->period;
      g_mutex_unlock(&state->mutex);
      timer->callback(timer, timer->data, delta);
      g_mutex_lock(&state->mutex);
      if (timer->repeat > 0 && --timer->repeat == 0)
        timer->removed = TRUE;
    }
  }
  for (link = &state->timers; *link;) {
    timer = *link;
    if (timer->removed) {
      *link = timer->next;
      g_free(timer);
    } else {
      link = &timer->next;
    }
  }
  state->scanning = FALSE;
  g_cond_broadcast(&state->scan_done);
done:
  g_mutex_unlock(&state->mutex);
  g_source_unref((GSource *)state);
}

static gboolean dispatch(GSource *source, GSourceFunc callback, gpointer data)
{
  (void)callback;
  (void)data;
  TimerScanFor((IvyTimerState *)source);
  return G_SOURCE_CONTINUE;
}

static void finalize(GSource *source)
{
  IvyTimerState *state = (IvyTimerState *)source;
  while (state->timers) {
    TimerId timer = state->timers;
    state->timers = timer->next;
    g_free(timer);
  }
  g_main_context_unref(state->context);
  g_cond_clear(&state->scan_done);
  g_mutex_clear(&state->mutex);
}

static GSourceFuncs source_funcs = {
  .prepare = prepare, .check = check, .dispatch = dispatch, .finalize = finalize
};

IvyTimerState *IvyGlibTimerStateCreate(GMainContext *context)
{
  IvyTimerState *state = (IvyTimerState *)
    g_source_new(&source_funcs, sizeof(*state));
  g_mutex_init(&state->mutex);
  g_cond_init(&state->scan_done);
  state->context = g_main_context_ref(context);
  g_source_set_name((GSource *)state, "Ivy timers");
  g_source_attach((GSource *)state, state->context);
  return state;
}

IvyTimerState *TimerStateCreate(void)
{
  GMainContext *context = g_main_context_ref_thread_default();
  IvyTimerState *state = IvyGlibTimerStateCreate(context);
  g_main_context_unref(context);
  return state;
}

IvyTimerState *TimerGetDefaultState(void)
{
  if (g_once_init_enter(&default_initialized)) {
    default_state = IvyGlibTimerStateCreate(g_main_context_default());
    g_once_init_leave(&default_initialized, 1);
  }
  return default_state;
}

void IvyGlibTimerSetStopped(IvyTimerState *state, gboolean stopped)
{
  state = normalize(state);
  g_mutex_lock(&state->mutex);
  state->stopped = stopped;
  g_mutex_unlock(&state->mutex);
  g_main_context_wakeup(state->context);
}

void TimerStateDestroy(IvyTimerState *state)
{
  if (!state || state == default_state)
    return;
  IvyGlibTimerSetStopped(state, TRUE);
  g_source_destroy((GSource *)state);
  g_mutex_lock(&state->mutex);
  while (state->scanning && !g_main_context_is_owner(state->context))
    g_cond_wait(&state->scan_done, &state->mutex);
  g_mutex_unlock(&state->mutex);
  g_source_unref((GSource *)state);
}

TimerId TimerRepeatAfterFor(IvyTimerState *state, int count, long timeout,
                           TimerCb callback, void *data)
{
  TimerId timer;
  gint64 now = g_get_monotonic_time();
  if (!callback || timeout < 0 || timeout > (G_MAXINT64 - now) / 1000)
    return NULL;
  state = normalize(state);
  timer = g_try_new0(struct _timer, 1);
  if (!timer)
    return NULL;
  timer->owner = state;
  timer->repeat = count;
  timer->period = (gint64)timeout * 1000;
  timer->when = now + timer->period;
  timer->callback = callback;
  timer->data = data;
  g_mutex_lock(&state->mutex);
  timer->next = state->timers;
  state->timers = timer;
  g_mutex_unlock(&state->mutex);
  g_main_context_wakeup(state->context);
  return timer;
}

TimerId TimerRepeatAfter(int count, long timeout, TimerCb callback, void *data)
{ return TimerRepeatAfterFor(NULL, count, timeout, callback, data); }

void TimerModify(TimerId timer, long timeout)
{
  IvyTimerState *state;
  gint64 now = g_get_monotonic_time();
  if (!timer || timeout < 0 || timeout > (G_MAXINT64 - now) / 1000)
    return;
  state = timer->owner;
  g_mutex_lock(&state->mutex);
  if (!timer->removed) {
    timer->period = (gint64)timeout * 1000;
    timer->when = now + timer->period;
  }
  g_mutex_unlock(&state->mutex);
  g_main_context_wakeup(state->context);
}

void TimerRemove(TimerId timer)
{
  IvyTimerState *state;
  if (!timer)
    return;
  state = timer->owner;
  g_mutex_lock(&state->mutex);
  timer->removed = TRUE;
  g_mutex_unlock(&state->mutex);
  g_main_context_wakeup(state->context);
}

struct timeval *TimerGetSmallestTimeoutFor(IvyTimerState *state)
{
  static _Thread_local struct timeval timeout;
  gint64 next, remaining;
  state = normalize(state);
  g_mutex_lock(&state->mutex);
  next = deadline(state);
  g_mutex_unlock(&state->mutex);
  if (next < 0)
    return NULL;
  remaining = MAX((gint64)0, next - g_get_monotonic_time());
  timeout.tv_sec = remaining / G_USEC_PER_SEC;
  timeout.tv_usec = remaining % G_USEC_PER_SEC;
  return &timeout;
}

struct timeval *TimerGetSmallestTimeout(void)
{ return TimerGetSmallestTimeoutFor(NULL); }
void TimerScan(void) { TimerScanFor(NULL); }
