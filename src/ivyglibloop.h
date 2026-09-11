/*
*	Ivy, C interface
*
*	Copyright (C) 1997-2000
*	Centre d'Études de la Navigation Aérienne
*
* 	Main loop based on the GTK Toolkit
*
*	Authors: François-Régis Colin <fcolin@cena.fr>
*
*	$Id: ivyglibloop.h 1231 2006-04-21 16:34:15Z fcolin $
* 
*	Please refer to file version.h for the
*	copyright notice regarding this software
*/

#ifndef IVYGLIBLOOP_H
#define IVYGLIBLOOP_H

#ifdef __cplusplus
extern "C" {
#endif

/* The contextual APIs are declared in ivy.h, ivychannel.h, ivyloop.h and
 * timer.h. New contexts attach to the creating thread's thread-default
 * GMainContext (or the global default). Legacy wrappers use the global default.
 * An external GLib/GTK loop can drive these sources directly. IvyChannelStopFor
 * only stops the selected Ivy state; it does not quit the application's loop.
 */
#ifdef __cplusplus
}
#endif

#endif /* IVYGLIBLOOP_H */
