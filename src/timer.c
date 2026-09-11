/*
 *	Ivy, C interface
 *
 *	Copyright (C) 1997-2000
 *	Centre d'Études de la Navigation Aérienne
 *
 *	Timers used in select based main loop
 *
 *	Authors: François-Régis Colin <fcolin@cena.dgac.fr>
 *
 *	$Id: timer.c 3591 2013-06-20 17:23:52Z bustico $
 * 
 *	Please refer to file version.h for the
 *	copyright notice regarding this software
 */

/* Module de gestion des timers autour d'un select */
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <stdlib.h>
#include <memory.h> 
#ifdef WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif
#include "list.h"
#include "timer.h"

#define BIGVALUE 2147483647
#define MILLISEC 1000

struct _timer {
	struct _timer *next;
	IvyTimerState *owner;
	int	repeat;
	unsigned long period;
	unsigned long when;
	TimerCb callback;
	void *user_data;
        unsigned char mark2Remove;
	};

struct _timer_state {
	struct timeval *timeoutptr;
	struct timeval selectTimeout;
	/* la prochaine echeance */
	unsigned long nextTimeout;
	/* liste des timers */
	TimerId timers;
};

static IvyTimerState default_timer_state = {
	NULL,
	{ BIGVALUE, 0 },
	BIGVALUE,
	NULL
};

static IvyTimerState *TimerNormalizeState(IvyTimerState *state)
{
	return state ? state : &default_timer_state;
}

static long currentTime()
{	
	unsigned long current;
#ifdef WIN32
	current = GetTickCount();
#else
	struct timeval stamp;
	gettimeofday( &stamp, NULL );
	current = stamp.tv_sec * MILLISEC + stamp.tv_usec/MILLISEC;
#endif
	return  current;
}
IvyTimerState *TimerGetDefaultState(void)
{
	return &default_timer_state;
}

IvyTimerState *TimerStateCreate(void)
{
	IvyTimerState *state = (IvyTimerState *)calloc(1, sizeof(*state));
	if (!state)
		return NULL;
	state->selectTimeout.tv_sec = BIGVALUE;
	state->selectTimeout.tv_usec = 0;
	state->nextTimeout = BIGVALUE;
	return state;
}

void TimerStateDestroy(IvyTimerState *state)
{
	TimerId timer;
	TimerId next;

	if (!state || state == &default_timer_state)
		return;

	IVY_LIST_EACH_SAFE(state->timers, timer, next) {
		IVY_LIST_REMOVE(state->timers, timer);
	}
	free(state);
}

static void SetNewTimeout(IvyTimerState *state, unsigned long current, unsigned long when )
{
	unsigned long ltime;
	state = TimerNormalizeState(state);
	ltime = (when <= current) ? 0 : when - current;
	state->nextTimeout = when;
	state->selectTimeout.tv_sec = ltime / MILLISEC;
	state->selectTimeout.tv_usec = (ltime - state->selectTimeout.tv_sec* MILLISEC) * MILLISEC;
	if ( state->timeoutptr == NULL )
				state->timeoutptr = &state->selectTimeout;
	/*printf("New timeout %lu\n", ltime );*/
}
static void AdjTimeout(IvyTimerState *state, unsigned long current)
{
	unsigned long newTimeout;
	TimerId timer;
	state = TimerNormalizeState(state);
	if ( state->timers )
	{
	/* recherche de la plus courte echeance dans la liste */
	newTimeout =  state->timers->when ; /* remise a la premiere valeur */
	IVY_LIST_EACH( state->timers , timer )
		{
		  if ((!timer->mark2Remove) && (timer->when < newTimeout  ))
		    newTimeout = timer->when;
		}
	SetNewTimeout( state, current, newTimeout );
	}
	else
	{
	state->timeoutptr = NULL;
	}
}

/* API */

TimerId TimerRepeatAfterFor( IvyTimerState *state, int count, long ltime, TimerCb cb, void *user_data )
{
	unsigned long stamp;
	TimerId timer;
	state = TimerNormalizeState(state);

	/* si y a rien a faire et ben on fait rien */
	if ( cb == NULL ) return NULL;

	IVY_LIST_ADD_START( state->timers, timer )
		timer->owner = state;
		timer->repeat = count;
		timer->callback = cb;
		timer->user_data = user_data;
		stamp = currentTime();
		timer->period = ltime;
		timer->when =  stamp + ltime;
		timer->mark2Remove = 0;
		if ( (timer->when < state->nextTimeout) || (state->timeoutptr == NULL))
			SetNewTimeout( state, stamp, timer->when );
	IVY_LIST_ADD_END( state->timers, timer )
	return timer;
}

TimerId TimerRepeatAfter( int count, long ltime, TimerCb cb, void *user_data )
{
	return TimerRepeatAfterFor(TimerGetDefaultState(), count, ltime, cb, user_data);
}

void TimerRemove( TimerId timer )
{
	unsigned long stamp;
	if (( !timer ) || (timer->mark2Remove)) return;
	//	IVY_LIST_REMOVE( timers, timer );
	timer->mark2Remove = 1;
	stamp = currentTime();
	AdjTimeout(timer->owner, stamp);
}
void TimerModify( TimerId timer, long ltime )
{
	unsigned long stamp;
	if (( !timer ) || (timer->mark2Remove)) return;

	stamp = currentTime();
	timer->period = ltime;
	timer->when = stamp + ltime;
	AdjTimeout(timer->owner, stamp);
}
/* Interface avec select */

struct timeval *TimerGetSmallestTimeoutFor(IvyTimerState *state)
{
	unsigned long stamp;
	state = TimerNormalizeState(state);
	/* recalcul du prochain timeout */
	stamp = currentTime();
	AdjTimeout( state, stamp );
	return state->timeoutptr;
}

struct timeval *TimerGetSmallestTimeout()
{
	return TimerGetSmallestTimeoutFor(TimerGetDefaultState());
}

void TimerScanFor(IvyTimerState *state)
{
	unsigned long stamp;
	TimerId timer;
	TimerId next;
	unsigned long delta;
	state = TimerNormalizeState(state);
	
	stamp = currentTime();

	/* recherche des timers echu dans la liste */
	IVY_LIST_EACH_SAFE( state->timers , timer, next )
	{
	  if ( timer->when <= stamp && (!timer->mark2Remove) )
	    {
	      delta = stamp - timer->when;
	      /* call callback */
	      (*timer->callback)( timer, timer->user_data, delta );
	    }
	}

	IVY_LIST_EACH_SAFE( state->timers , timer, next )
	{
	  if (timer->mark2Remove) {
	    IVY_LIST_REMOVE( state->timers, timer );
	  }
	  else if ( timer->when <= stamp )
	    {
	      if ( timer->repeat == TIMER_LOOP || --(timer->repeat) )
		{
		  timer->when = stamp + timer->period;
		}
	      else
		{
		  IVY_LIST_REMOVE( state->timers, timer );
		}
	    }
	}
	
}

void TimerScan()
{
	TimerScanFor(TimerGetDefaultState());
}

#ifdef WIN32

#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
  #define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64
#else
  #define DELTA_EPOCH_IN_MICROSECS  11644473600000000ULL
#endif


int gettimeofday(struct timeval *tv, struct timezone *tz)
{
  FILETIME ft;
  unsigned __int64 tmpres = 0;
  static int tzflag = 0;

  if (NULL != tv)
  {
    GetSystemTimeAsFileTime(&ft);

    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;

    tmpres /= 10;  /*convert into microseconds*/
    /*converting file time to unix epoch*/
    tmpres -= DELTA_EPOCH_IN_MICROSECS; 
    tv->tv_sec = (long)(tmpres / 1000000UL);
    tv->tv_usec = (long)(tmpres % 1000000UL);
  }

  if (NULL != tz)
  {
    if (!tzflag)
    {
      _tzset();
      tzflag++;
    }
    tz->tz_minuteswest = _timezone / 60;
    tz->tz_dsttime = _daylight;
  }

  return 0;
}

#endif
