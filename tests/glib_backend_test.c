#include "ivy.h"
#include "ivyloop.h"
#include "timer.h"

#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    abort(); \
  } \
} while (0)

struct fixture {
  GMainContext *main_context;
  GMainLoop *loop;
  IvyContext *a, *b;
  IvyChannelState *channels;
  Channel channel;
  int sockets[2];
  gint received_a, received_b, ticks, controls, reads, writes, deletes;
  gint timer_modified, cancelled_calls;
  gint ready;
};

static void wait_count(gint *value, gint target)
{
  gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (g_atomic_int_get(value) < target && g_get_monotonic_time() < end)
    g_usleep(1000);
  CHECK(g_atomic_int_get(value) >= target);
}

static void message(IvyClientPtr app, void *data, int argc, char **argv)
{
  (void)app;
  CHECK(argc == 1 && strcmp(argv[0], "hello") == 0);
  g_atomic_int_inc((gint *)data);
}

static void broadcast(TimerId timer, void *data, unsigned long delta)
{
  struct fixture *f = data;
  (void)timer;
  (void)delta;
  CHECK(g_main_context_is_owner(f->main_context));
  g_atomic_int_inc(&f->ticks);
  CHECK(IvyContextSendMsg(f->a, "glib hello") >= 0);
  CHECK(IvyContextSendMsg(f->b, "glib hello") >= 0);
}

static gboolean ready(gpointer data)
{
  struct fixture *f = data;
  g_atomic_int_set(&f->ready, 1);
  return G_SOURCE_REMOVE;
}

static void *run_glib(void *data)
{
  struct fixture *f = data;
  GSource *source = g_idle_source_new();
  g_source_set_callback(source, ready, f, NULL);
  g_source_attach(source, f->main_context);
  g_source_unref(source);
  g_main_loop_run(f->loop);
  return NULL;
}

static void control(void *data)
{
  struct fixture *f = data;
  CHECK(IvyChannelIsLoopThreadFor(f->channels));
  CHECK(IvyChannelLoopIsActiveFor(f->channels));
  g_atomic_int_inc(&f->controls);
}

static void on_delete(void *data)
{
  struct fixture *f = data;
  CHECK(close(f->sockets[0]) == 0);
  g_atomic_int_inc(&f->deletes);
}

static void on_read(Channel channel, IVY_HANDLE fd, void *data)
{
  struct fixture *f = data;
  char byte;
  CHECK(IvyChannelIsLoopThreadFor(f->channels));
  CHECK(read(fd, &byte, 1) == 1);
  CHECK(byte == 'x');
  g_atomic_int_inc(&f->reads);
  /* Removing a channel during read must suppress its ready write callback. */
  IvyChannelRemove(channel);
}

static void on_write(Channel channel, IVY_HANDLE fd, void *data)
{
  struct fixture *f = data;
  CHECK(IvyChannelIsLoopThreadFor(f->channels));
  CHECK(write(fd, "y", 1) == 1);
  IvyChannelClearWritableEvent(channel);
  g_atomic_int_inc(&f->writes);
}

static void modified_timer(TimerId timer, void *data, unsigned long delta)
{
  struct fixture *f = data;
  (void)delta;
  CHECK(g_main_context_is_owner(f->main_context));
  if (g_atomic_int_add(&f->timer_modified, 1) == 0)
    TimerModify(timer, 1);
  else
    TimerRemove(timer);
}

static void cancelled_timer(TimerId timer, void *data, unsigned long delta)
{
  (void)timer;
  (void)delta;
  g_atomic_int_inc((gint *)data);
}

/* Two public contexts exchanging real Ivy traffic under an application-owned
 * loop. Repeat on a private main context to catch accidental g_*_add() use. */
static void external_loop_test(gboolean private_context, const char *bus)
{
  struct fixture f = {0};
  pthread_t thread;
  TimerId repeating, modified, cancelled;
  char byte;
  gint ticks;

  f.main_context = private_context ? g_main_context_new() :
    g_main_context_ref(g_main_context_default());
  if (private_context)
    g_main_context_push_thread_default(f.main_context);
  f.loop = g_main_loop_new(f.main_context, FALSE);
  f.a = IvyContextCreate("glib-a", NULL, NULL, NULL, NULL, NULL);
  f.b = IvyContextCreate("glib-b", NULL, NULL, NULL, NULL, NULL);
  CHECK(f.a && f.b);
  CHECK(IvyContextBindMsg(f.a, message, &f.received_a, "^glib (hello)$"));
  CHECK(IvyContextBindMsg(f.b, message, &f.received_b, "^glib (hello)$"));
  CHECK(IvyContextStart(f.a, bus) == IVY_OK);
  CHECK(IvyContextStart(f.b, bus) == IVY_OK);
  f.channels = IvyChannelStateCreate();
  CHECK(IvyChannelInitFor(f.channels) == 0);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f.sockets) == 0);
  f.channel = IvyChannelAddFor(f.channels, f.sockets[0], &f,
                              on_delete, on_read, on_write);
  CHECK(f.channel);
  if (private_context)
    g_main_context_pop_thread_default(f.main_context);

  CHECK(pthread_create(&thread, NULL, run_glib, &f) == 0);
  wait_count(&f.ready, 1);
  CHECK(IvyChannelLoopIsActiveFor(f.channels));
  CHECK(!IvyChannelIsLoopThreadFor(f.channels));
  /* These synchronous contextual timer requests must wake and run on GLib. */
  repeating = IvyContextTimerRepeatAfter(f.a, TIMER_LOOP, 20, broadcast, &f);
  CHECK(repeating);
  modified = IvyContextTimerRepeatAfter(f.b, TIMER_LOOP, 60000, modified_timer, &f);
  CHECK(modified);
  TimerModify(modified, 1);
  wait_count(&f.timer_modified, 2);
  cancelled = IvyContextTimerRepeatAfter(f.b, 1, 60000,
                                         cancelled_timer, &f.cancelled_calls);
  CHECK(cancelled);
  TimerRemove(cancelled);
  wait_count(&f.received_a, 1);
  wait_count(&f.received_b, 1);

  CHECK(IvyChannelPostControlFor(f.channels, control, &f) == 0);
  wait_count(&f.controls, 1);
  IvyChannelAddWritableEvent(f.channel);
  wait_count(&f.writes, 1);
  CHECK(read(f.sockets[1], &byte, 1) == 1 && byte == 'y');
  /* Repeated registration is idempotent. */
  IvyChannelClearWritableEvent(f.channel);
  IvyChannelClearWritableEvent(f.channel);
  CHECK(write(f.sockets[1], "x", 1) == 1);
  wait_count(&f.deletes, 1);
  CHECK(g_atomic_int_get(&f.reads) == 1);
  CHECK(g_atomic_int_get(&f.writes) == 1);
  CHECK(close(f.sockets[1]) == 0);

  CHECK(IvyContextStop(f.a) == IVY_OK);
  ticks = g_atomic_int_get(&f.ticks);
  /* A stopped state must not stop the application's loop or the other bus. */
  CHECK(IvyContextTimerRepeatAfter(f.b, 1, 1, cancelled_timer, &f.controls));
  wait_count(&f.controls, 2);
  CHECK(g_atomic_int_get(&f.ticks) == ticks);
  CHECK(g_atomic_int_get(&f.cancelled_calls) == 0);
  CHECK(g_main_loop_is_running(f.loop));
  CHECK(IvyContextStop(f.b) == IVY_OK);
  CHECK(IvyContextDestroy(f.a) == IVY_OK);
  CHECK(IvyContextDestroy(f.b) == IVY_OK);

  /* Stop after an external loop exits must not wait for a cached loop thread. */
  g_main_loop_quit(f.loop);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(!IvyChannelLoopIsActiveFor(f.channels));
  IvyChannelStateDestroy(f.channels);
  CHECK(g_atomic_int_get(&f.deletes) == 1);
  g_main_loop_unref(f.loop);
  g_main_context_unref(f.main_context);
}

static void stop_timer(TimerId timer, void *data, unsigned long delta)
{
  (void)timer;
  (void)delta;
  IvyChannelStopFor(data);
}

static void *run_ivy(void *data)
{
  IvyMainLoopFor(data);
  return NULL;
}

static void loop_and_idle_test(void)
{
  GMainContext *contexts[2];
  IvyChannelState *states[2];
  pthread_t threads[2];
  gint counts[2] = {0, 0};
  int i;
  for (i = 0; i < 2; ++i) {
    contexts[i] = g_main_context_new();
    g_main_context_push_thread_default(contexts[i]);
    states[i] = IvyChannelStateCreate();
    CHECK(IvyChannelInitFor(states[i]) == 0);
    CHECK(TimerRepeatAfterFor(IvyChannelGetTimerState(states[i]), 1, 0,
                             cancelled_timer, &counts[i]));
    IvyIdleFor(states[i]);
    CHECK(counts[i] == 1);
    CHECK(TimerRepeatAfterFor(IvyChannelGetTimerState(states[i]), 1, 20,
                             stop_timer, states[i]));
    g_main_context_pop_thread_default(contexts[i]);
    CHECK(pthread_create(&threads[i], NULL, run_ivy, states[i]) == 0);
  }
  for (i = 0; i < 2; ++i) {
    CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(!IvyChannelLoopIsActiveFor(states[i]));
    IvyIdleFor(states[i]);
    CHECK(!IvyChannelLoopIsActiveFor(states[i]));
    IvyChannelStateDestroy(states[i]);
    g_main_context_unref(contexts[i]);
  }
  /* Historical channel/loop/timer entry points share one default state. */
  IvyChannelInit();
  CHECK(TimerRepeatAfter(1, 0, stop_timer, IvyChannelGetDefaultState()));
  IvyMainLoop();
  CHECK(!IvyChannelLoopIsActive());
}

static void before_poll(void *data)
{ *(int *)data = 0; }
static void after_poll(void *data)
{ *(int *)data = 1; }
static void check_hook(TimerId timer, void *data, unsigned long delta)
{
  (void)timer;
  (void)delta;
  CHECK(*(int *)data == 1);
  *(int *)data = 2;
}

static void idle_io_test(void)
{
  struct fixture f = {0};
  int hook = 1;
  f.main_context = g_main_context_new();
  g_main_context_push_thread_default(f.main_context);
  f.channels = IvyChannelStateCreate();
  CHECK(IvyChannelInitFor(f.channels) == 0);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f.sockets) == 0);
  f.channel = IvyChannelAddFor(f.channels, f.sockets[0], &f,
                              on_delete, on_read, on_write);
  CHECK(f.channel);
  CHECK(write(f.sockets[1], "x", 1) == 1);
  IvyChannelAddWritableEventFor(f.channels, f.channel);
  IvyChannelAddWritableEventFor(f.channels, f.channel);
  IvyIdleFor(f.channels);
  IvyIdleFor(f.channels);
  CHECK(f.reads == 1 && f.writes == 0 && f.deletes == 1);
  CHECK(close(f.sockets[1]) == 0);
  IvySetBeforeSelectHookFor(f.channels, before_poll, &hook);
  IvySetAfterSelectHookFor(f.channels, after_poll, &hook);
  CHECK(TimerRepeatAfterFor(IvyChannelGetTimerState(f.channels), 1, 0,
                            check_hook, &hook));
  IvyIdleFor(f.channels);
  CHECK(hook == 2);
  IvyChannelStateDestroy(f.channels);
  g_main_context_pop_thread_default(f.main_context);
  g_main_context_unref(f.main_context);
}

int main(int argc, char **argv)
{
  CHECK(argc == 2);
  alarm(25);
  external_loop_test(FALSE, argv[1]);
  external_loop_test(TRUE, argv[1]);
  loop_and_idle_test();
  idle_io_test();
  alarm(0);
  puts("GLib backend tests passed");
  return 0;
}
