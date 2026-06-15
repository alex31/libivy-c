/*
 *	Ivy unbind Test
 *
 *	Copyright (C) 1997-2004
 *	Centre d'Études de la Navigation Aérienne
 *
 *	Authors: Yannick Jestin <jestin@cena.fr>
 *
 *	Please refer to file ../src/version.h for the
 *	copyright notice regarding this software
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ivy.h"
#define REGEXP "^ub"

static IvyContext *test_ctx = NULL;

void Callback (IvyClientPtr app, void *user_data, int argc, char *argv[]) {
  MsgRcvPtr *ptr = (MsgRcvPtr *) user_data;
  printf ("%s sent unbind message, unbinding to %s\n",
      IvyContextGetApplicationName(test_ctx, app),REGEXP);
  IvyContextUnbindMsg(test_ctx, *ptr);
}

int main(int argc, char *argv[]) {
  MsgRcvPtr ptr;
  test_ctx = IvyContextCreate("TestUnbind","TestUnbind Ready",NULL,NULL,NULL,NULL);
  if (test_ctx == NULL) {
    fprintf(stderr, "IvyContextCreate failed: %d\n", IvyGetLastError());
    return 1;
  }
  ptr=IvyContextBindMsg(test_ctx, Callback,&ptr,REGEXP);
  printf("bound to %s\n",REGEXP);
  if (IvyContextStart(test_ctx, NULL) != IVY_OK) {
    fprintf(stderr, "IvyContextStart failed: %d\n", IvyGetLastError());
    IvyContextDestroy(test_ctx);
    return 1;
  }
  IvyContextMainLoop(test_ctx);
  IvyContextDestroy(test_ctx);
  return 0;
}
