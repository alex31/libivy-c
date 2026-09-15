#pragma once
#include "timer.h"

/* Select backend only: stop selecting timer callbacks when the loop stops. */
void IvyTimerScanWhileFor(IvyTimerState *state,
                         int (*keep_running)(void *), void *data);
