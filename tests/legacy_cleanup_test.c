#include "ivy.h"
#include "ivyloop.h"
#include "timer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>
#endif

static int received, disconnected, stale_callbacks;

static void stale_control(void *data)
{
  (void)data;
  ++stale_callbacks;
}

static void stale_timer(TimerId timer, void *data, unsigned long delta)
{
  (void)timer;
  (void)delta;
  stale_control(data);
}

static void application(IvyClientPtr peer, void *data, IvyApplicationEvent event)
{
  (void)data;
  if (event == IvyApplicationDisconnected) {
    /* Destruction callbacks must still see the old, stopped default context. */
    assert(strcmp(IvyGetApplicationName(peer), "cleanup-peer") == 0);
    assert(IvySendMsg("after-stop") == IVY_ESTOPPED);
    ++disconnected;
  }
}

static void message(IvyClientPtr peer, void *data, int argc, char **argv)
{
  (void)peer;
  (void)data;
  assert(argc == 1 && strcmp(argv[0], "hello") == 0);
  ++received;
  assert(IvyStop() == IVY_OK);
}

static void ignore_message(IvyClientPtr peer, void *data, int argc, char **argv)
{
  (void)peer;
  (void)data;
  (void)argc;
  (void)argv;
}

static int open_descriptors(void)
{
#ifdef __linux__
  DIR *directory = opendir("/proc/self/fd");
  struct dirent *entry;
  int count = 0;
  assert(directory);
  while ((entry = readdir(directory)))
    if (entry->d_name[0] != '.')
      ++count;
  assert(closedir(directory) == 0);
  return count;
#else
  return 0;
#endif
}

int main(int argc, char **argv)
{
  int initial_fds, cycle;
  assert(argc == 2);
  alarm(30);
  /* Warm up the GLib default context, which intentionally retains its wake fd. */
  assert(IvyInit("warmup", NULL, NULL, NULL, NULL, NULL) == IVY_OK);
  assert(IvyTerminate() == IVY_OK);
  initial_fds = open_descriptors();

  for (cycle = 0; cycle < 20; ++cycle) {
    IvyContext *peer;
    int attempts;
    received = disconnected = 0;
    assert(IvyInit("cleanup-legacy", NULL, application, NULL, NULL, NULL) == IVY_OK);
    IvyIdle();
    assert(stale_callbacks == 0);
    assert(IvyBindMsg(message, NULL, "^cleanup (hello)$"));
    assert(IvyStart(argv[1]) == IVY_OK);
    peer = IvyContextCreate("cleanup-peer", NULL, NULL, NULL, NULL, NULL);
    assert(peer);
    /* Leave a remote subscription in the legacy send dictionary at shutdown. */
    assert(IvyContextBindMsg(peer, ignore_message, NULL, "^unused (.*)$"));
    assert(IvyContextStart(peer, argv[1]) == IVY_OK);
    for (attempts = 0; !received && attempts < 2000; ++attempts) {
      IvyIdle();
      IvyContextIdle(peer);
      if (!received)
        assert(IvyContextSendMsg(peer, "cleanup hello") >= 0);
      usleep(1000);
    }
    assert(received == 1);
    assert(TimerRepeatAfter(1, 0, stale_timer, NULL));
    assert(IvyChannelPostControl(stale_control, NULL) == 0);
    assert(IvyTerminate() == IVY_OK);
    assert(disconnected == 1);
    assert(IvyTerminate() == IVY_OK);
    assert(IvyContextStop(peer) == IVY_OK);
    assert(IvyContextDestroy(peer) == IVY_OK);
    assert(stale_callbacks == 0);
    assert(open_descriptors() == initial_fds);
  }
  puts("legacy cleanup and reinitialization passed (20 cycles)");
  return 0;
}
