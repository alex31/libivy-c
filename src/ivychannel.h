/*
 *	Ivy, C interface
 *
 *	Copyright (C) 1997-2000
 *	Centre d'Études de la Navigation Aérienne
 *
 * 	Basic I/O handling
 *
 *	Authors: François-Régis Colin <fcolin@cena.dgac.fr>
 *
 *	$Id: ivychannel.h 3460 2011-01-24 13:39:15Z bustico $
 * 
 *	Please refer to file version.h for the
 *	copyright notice regarding this software
 *
 */
#ifndef _IVYCHANNEL_H
#define _IVYCHANNEL_H

#ifdef __cplusplus
extern "C" {
#endif
	
/* general Handle */

#ifdef WIN32
#include <windows.h>
#define IVY_HANDLE SOCKET
#else
#define IVY_HANDLE int
#endif

typedef struct _ivy_channel_state IvyChannelState;
typedef struct _timer_state IvyTimerState;
typedef struct _channel *Channel;
/* callback declenche par la gestion de boucle  sur evenement exception sur le canal */
typedef void (*ChannelHandleDelete)( void *data );
/* callback declenche par la gestion de boucle sur donnees pretes sur le canal */
typedef void (*ChannelHandleRead)( Channel channel, IVY_HANDLE fd, void *data);
typedef void (*ChannelHandleWrite)( Channel channel, IVY_HANDLE fd, void *data);
typedef void (*IvyControlCallback)(void *data);

/* fonction appele par le bus pour initialisation */
extern IvyChannelState *IvyChannelStateCreate(void);
extern void IvyChannelStateDestroy(IvyChannelState *state);
extern IvyChannelState *IvyChannelGetDefaultState(void);
extern IvyTimerState *IvyChannelGetTimerState(IvyChannelState *state);

extern int IvyChannelInitFor(IvyChannelState *state);
extern void IvyChannelInit(void);

extern void IvyChannelStopFor(IvyChannelState *state);
extern void IvyChannelStop (void);
extern void IvyChannelWakeFor(IvyChannelState *state);
extern void IvyChannelWake (void);
extern int IvyChannelPostControlFor(IvyChannelState *state, IvyControlCallback callback, void *data);
extern int IvyChannelPostControl(IvyControlCallback callback, void *data);
extern int IvyChannelLoopIsActiveFor(IvyChannelState *state);
extern int IvyChannelLoopIsActive(void);
extern int IvyChannelIsLoopThreadFor(IvyChannelState *state);
extern int IvyChannelIsLoopThread(void);

#ifdef IVY_TESTING
enum {
	IVY_TEST_CHANNEL_INIT_FAIL_NONE = 0,
	IVY_TEST_CHANNEL_INIT_FAIL_CONTROL = 1,
	IVY_TEST_CHANNEL_INIT_FAIL_WAKEUP = 2
};
extern void IvyTestingChannelInitFailStep(int step);
#endif

/* fonction appele par le bus pour mise en place des callback sur le canal */
extern Channel IvyChannelAddFor(
	IvyChannelState *state,
	IVY_HANDLE fd,
	void *data,
	ChannelHandleDelete handle_delete,
	ChannelHandleRead handle_read,
	ChannelHandleWrite handle_write
);
extern Channel IvyChannelAdd(
	IVY_HANDLE fd,
	void *data,
	ChannelHandleDelete handle_delete,
	ChannelHandleRead handle_read,
	ChannelHandleWrite handle_write
);

/* fonction appele par le bus pour suppression des callback sur le canal */
extern void IvyChannelRemove( Channel channel );


#ifdef __cplusplus
}
#endif

#endif
