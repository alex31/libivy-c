/* Private GLib backend integration. */
#ifndef IVY_GLIB_TIMER_H
#define IVY_GLIB_TIMER_H
#include <glib.h>
#include "timer.h"
IvyTimerState *IvyGlibTimerStateCreate(GMainContext *context);
void IvyGlibTimerSetStopped(IvyTimerState *state, gboolean stopped);
#endif
