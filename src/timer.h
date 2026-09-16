/*
 *	Ivy, C interface
 *
 *	Copyright (C) 1997-2000
 *	Centre d'Études de la Navigation Aérienne
 *
 *	Timers for select-based main loop
 *
 *	Authors: François-Régis Colin <fcolin@cena.dgac.fr>
 *
 *	$Id: timer.h 3591 2013-06-20 17:23:52Z bustico $
 * 
 *	Please refer to file version.h for the
 *	copyright notice regarding this software
 */
/**
 * @file timer.h
 * @brief Timer handles and low-level event-loop timer operations.
 * @ingroup ivy_c_timers
 * Times are expressed in milliseconds. Applications normally create timers with
 * IvyContextTimerRepeatAfter(). With the native select backend, call low-level
 * operations on the owner loop thread, or while that loop is not running.
 * A timer callback may modify or remove its own timer. Expired/removed handles
 * must not be reused after the loop has reclaimed them.
 */
#ifndef IVYTIMER_H
#define IVYTIMER_H


#ifdef __cplusplus
extern "C" {
#endif
	
/**
 * @defgroup ivy_c_timers C low-level timer API
 * @ingroup ivy_c_api
 * @brief Timer handles and backend operations; C++ timers use ivy::Bus::bind_event().
 * @{
 */

/** @brief Opaque timer collection associated with one event loop. */
typedef struct _timer_state IvyTimerState;
/** @brief Borrowed timer handle, valid until removal or final expiry is processed. */
typedef struct _timer *TimerId;
/** @brief Timer callback executed by the owner loop.
 * @param id Timer being dispatched.
 * @param user_data User pointer supplied at timer creation.
 * @param delta Lateness relative to the scheduled expiry, in milliseconds.
 */
typedef void (*TimerCb)( TimerId id , void *user_data, unsigned long delta );

/* API  le temps est en millisecondes */
/** @brief Repeat until explicitly removed. */
#define TIMER_LOOP -1
/** @brief Create an independent timer collection.
 * @return Owned state, or NULL on failure. GLib attaches it to the thread-default context.
 */
IvyTimerState *TimerStateCreate(void);
/** @brief Release a timer collection and its remaining timers.
 * @param state Owned collection with no active dispatch; NULL and the default state are ignored.
 */
void TimerStateDestroy(IvyTimerState *state);
/** @brief Access the backend's default timer collection.
 * @return Borrowed backend-owned state; do not destroy it.
 */
IvyTimerState *TimerGetDefaultState(void);
/** @brief Create a timer in a particular collection.
 * @param state Owner collection, or NULL for the backend default.
 * @param count Positive invocation count, or TIMER_LOOP to repeat indefinitely.
 * @param timeout Nonnegative interval in milliseconds.
 * @param cb Non-NULL callback.
 * @param user_data Borrowed user pointer supplied to the callback.
 * @return Timer handle, or NULL on failure. This low-level API does not perform
 * all the validation provided by the context/C++ timer APIs.
 */
TimerId TimerRepeatAfterFor(IvyTimerState *state, int count, long timeout, TimerCb cb, void *user_data );
/** @brief Create a timer in the backend's default collection.
 * @param count Positive invocation count, or TIMER_LOOP.
 * @param timeout Nonnegative interval in milliseconds.
 * @param cb Non-NULL callback.
 * @param user_data Borrowed user pointer supplied to the callback.
 * @return Same result as TimerRepeatAfterFor().
 */
TimerId TimerRepeatAfter( int count, long timeout, TimerCb cb, void *user_data );

/** @brief Restart a live timer's deadline with a new interval.
 * @param id Live timer handle; NULL is ignored.
 * @param timeout Nonnegative interval in milliseconds.
 * The remaining invocation count is preserved.
 */
void TimerModify( TimerId id, long timeout );

/** @brief Mark a timer for removal; a callback already executing may finish.
 * @param id Live timer handle; NULL is ignored. Actual reclamation occurs in the loop.
 */
void TimerRemove( TimerId id );


 //  implemetation of gettimeofday for windows
#if 0 //def WIN32
#include "time.h"
struct timezone 
{
  int  tz_minuteswest; /* minutes W of Greenwich */
  int  tz_dsttime;     /* type of dst correction */
};
int gettimeofday(struct timeval *tv, struct timezone *tz);
#endif

/* Interface avec select */

/** @brief Find the delay until the next deadline in a timer collection.
 * @param state Collection to inspect, or NULL for the backend default.
 * @return Borrowed timeval, or NULL when no timer deadline is available. Copy
 * the value to retain it across later timer operations or timeout queries.
 */
struct timeval *TimerGetSmallestTimeoutFor(IvyTimerState *state);
/** @brief Find the next deadline in the default collection.
 * @return Borrowed timeval, or NULL; same lifetime as TimerGetSmallestTimeoutFor().
 */
struct timeval *TimerGetSmallestTimeout();

/** @brief Dispatch due callbacks and reclaim retired timers in a collection.
 * @param state Collection to service, or NULL for the backend default.
 */
void TimerScanFor(IvyTimerState *state);
/** @brief Dispatch due callbacks and reclaim retired timers in the default collection. */
void TimerScan();
/** @} */
#ifdef __cplusplus
}
#endif
#endif
