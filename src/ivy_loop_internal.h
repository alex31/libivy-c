#pragma once
#include "ivychannel.h"

/* The context mutex serializes acquisition with timer creation and stop.
 * After successful acquisition, run and release on that same thread. Stop the
 * context before release so synchronous controls cannot arrive after draining. */
int IvyLoopAcquireFor(IvyChannelState *state);
int IvyLoopRunOwnedFor(IvyChannelState *state);
void IvyLoopReleaseFor(IvyChannelState *state);
