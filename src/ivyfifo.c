#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef WIN32
#include <windows.h>
#else
#include <errno.h>
#include <sys/socket.h>
#endif
#include <stdio.h> // DEBUG, pour printf
#include "ivyfifo.h"
#include "ivy.h"
#include <limits.h>
#include "ivysocket.h"
#include "param.h"



#define 	MIN(a, b)   ((a) > (b) ? (b) : (a))


static int IvyFifoRealloc(IvyFifoBuffer *f, unsigned int neededSize);

static unsigned int IvyFifoGenericRead(IvyFifoBuffer *f, const unsigned int buf_size, 
				       void (*func)(void*, void*, int), void* dest);

static void IvyFifoDrain(IvyFifoBuffer *f, int size);

/* static unsigned char IvyFifoPeek(IvyFifoBuffer *f, int offs); */



int IvyFifoInit(IvyFifoBuffer *f)
{
  f->buffer = (char *) malloc(IVY_FIFO_ALLOC_SIZE);
  if (!f->buffer) {
    f->wptr = f->rptr = f->end = NULL;
    f->full = 1;
    return -1;
  }
  f->wptr = f->rptr = f->buffer;
  f->end = f->buffer + IVY_FIFO_ALLOC_SIZE;
  f->full = 0;
  return 0;
}

void IvyFifoFree (IvyFifoBuffer *f)
{
  free(f->buffer);
  f->wptr = f->rptr = f->buffer = NULL;
}


IvyFifoBuffer* IvyFifoNew (void)
{
  IvyFifoBuffer* ifb = (IvyFifoBuffer*) malloc (sizeof (IvyFifoBuffer));
  if (!ifb)
    return NULL;
  if (IvyFifoInit (ifb) != 0) {
    free (ifb);
    return NULL;
  }
  return (ifb);
}

void IvyFifoDelete (IvyFifoBuffer *f)
{
  IvyFifoFree (f);
  free (f);
}


unsigned int IvyFifoSize (const IvyFifoBuffer  *f)
{
  return (f->end - f->buffer);
}

unsigned int IvyFifoLength (const IvyFifoBuffer  *f)
{
  long size = f->wptr - f->rptr;
  if (size < 0)
    size += f->end - f->buffer;
  return size;
}

unsigned int IvyFifoAvail(const IvyFifoBuffer  *f)
{
  return (IvyFifoSize (f)- IvyFifoLength (f));
}

unsigned int IvyFifoRead (IvyFifoBuffer *f, char *buf, unsigned int buf_size)
{
  return IvyFifoGenericRead(f, buf_size, NULL, buf);
}

static int IvyFifoRealloc(IvyFifoBuffer *f, unsigned int needed)
{
  unsigned int capacity;
  unsigned int length = IvyFifoLength(f);
  unsigned int first;
  char *buffer;

  /* One byte remains unused to distinguish a full ring from an empty one. */
  if (needed >= IVY_FIFO_MAX_ALLOC_SIZE) {
    f->full = 1;
    return IVY_EFIFOFULL;
  }
  capacity = ((needed / IVY_FIFO_ALLOC_SIZE) + 1) * IVY_FIFO_ALLOC_SIZE;
  if (capacity <= IvyFifoSize(f))
    return IVY_OK;
  buffer = (char *)malloc(capacity);
  if (!buffer) {
    f->full = 1;
    return IVY_ENOMEM;
  }
  first = MIN(length, (unsigned int)(f->end - f->rptr));
  memcpy(buffer, f->rptr, first);
  memcpy(buffer + first, f->buffer, length - first);
  free(f->buffer);
  f->buffer = f->rptr = buffer;
  f->wptr = buffer + length;
  f->end = buffer + capacity;
  f->full = 0;
  return IVY_OK;
}

int IvyFifoWriteChecked(IvyFifoBuffer *f, const char *buf, unsigned int size)
{
  unsigned int length;
  int status;
  if (!f || !f->buffer || (!buf && size))
    return IVY_EINVAL;
  length = IvyFifoLength(f);
  if (size > UINT_MAX - length) {
    f->full = 1;
    return IVY_EFIFOFULL;
  }
  if (size >= IvyFifoAvail(f)) {
    status = IvyFifoRealloc(f, size + length);
    if (status != IVY_OK)
      return status;
  }
  while (size) {
    unsigned int n = MIN((unsigned int)(f->end - f->wptr), size);
    memcpy(f->wptr, buf, n);
    f->wptr += n;
    if (f->wptr == f->end)
      f->wptr = f->buffer;
    buf += n;
    size -= n;
  }
  f->full = 0;
  return IVY_OK;
}

void IvyFifoWrite(IvyFifoBuffer *f, const char *buf, unsigned int size)
{
  (void)IvyFifoWriteChecked(f, buf, size);
}


unsigned int IvyFifoGenericRead (IvyFifoBuffer *f, const unsigned int buf_size, void (*func)(void*, void*, int), void* dest)
{
  unsigned int bytesToRead, retV;
  retV = bytesToRead = MIN(buf_size, IvyFifoLength(f));
  
  do {
    unsigned int len = MIN((unsigned int)(f->end - f->rptr), bytesToRead);
    if (func) {
      func (dest, f->rptr, len);
    } else {
      memcpy(dest, f->rptr, len);
      dest = (unsigned char*)dest + len;
    }
    IvyFifoDrain(f, len);
    bytesToRead -= len;
  } while (bytesToRead > 0);
  return (retV);
}


int IvyFifoFlush(IvyFifoBuffer *f, int fd, unsigned int *remaining, int *system_error)
{
  int status = IVY_OK;
  *system_error = 0;
  while (IvyFifoLength(f)) {
    unsigned int count = MIN((unsigned int)(f->end - f->rptr), IvyFifoLength(f));
#ifdef WIN32
    int sent = send(fd, f->rptr, count, IVY_MSG_NOSIGNAL);
    if (sent == SOCKET_ERROR) {
      int error = WSAGetLastError();
      if (error == WSAEWOULDBLOCK || error == WSAEINTR)
        break;
#else
    ssize_t sent = send(fd, f->rptr, count, MSG_DONTWAIT | IVY_MSG_NOSIGNAL);
    if (sent < 0) {
      int error = errno;
      if (error == EWOULDBLOCK || error == EAGAIN || error == EINTR)
        break;
#endif
      *system_error = error;
      status = IVY_EIO;
      break;
    }
    if (sent == 0) {
      status = IVY_EIO;
      break;
    }
    IvyFifoDrain(f, (int)sent);
    if ((unsigned int)sent < count)
      break;
  }
  *remaining = IvyFifoLength(f);
  return status;
}

unsigned int IvyFifoSendSocket(IvyFifoBuffer *f, const int fd)
{
  unsigned int remaining;
  int system_error;
  (void)IvyFifoFlush(f, fd, &remaining, &system_error);
  return remaining;
}


void IvyFifoDrain (IvyFifoBuffer *f, int size)
{
  f->rptr += size;
  if (f->rptr >= f->end)
    f->rptr -= f->end - f->buffer;
  if (size > 0) {
    f->full = 0;
  }
}

int IvyFifoIsFull (const IvyFifoBuffer  *f) 
{
  return (f->full);
}

/* unsigned char IvyFifoPeek(IvyFifoBuffer *f, int offs) */
/* { */
/*     unsigned char *ptr = f->rptr + offs; */
/*     if (ptr >= f->end) */
/*         ptr -= f->end - f->buffer; */
/*     return *ptr; */
/* } */


//#define TEST_UNITAIRE 1
#ifdef TEST_UNITAIRE
int main (int argc, char**argv)
{
  if (1 == 2)  {
    IvyFifoBuffer ifb;
    unsigned char toRead [4096];
    unsigned char toWrite [8192];
    int (i);
    
    for (i=0; i< sizeof (toRead); i++) {
      toRead[i] = (char) (i%10)+48;
    }
    
    
    IvyFifoInit (&ifb);
    IvyFifoWrite(&ifb, toRead, 4096);
    printf ("DBG> fifo size=%u, length=%u\n", IvyFifoSize(&ifb), IvyFifoLength(&ifb));
    IvyFifoWrite(&ifb, toRead, 1024);
    printf ("DBG> fifo size=%u, length=%u\n", IvyFifoSize(&ifb), IvyFifoLength(&ifb));
    unsigned int nbRead=IvyFifoRead(&ifb, toWrite, sizeof (toWrite));
    //  int nbRead=IvyFifoRead(&ifb, toWrite, 1024);
    printf ("DBG> ndRead=%u, fifo size=%u, length=%u\n", nbRead, IvyFifoSize(&ifb), IvyFifoLength(&ifb));
  }


  {
    IvyFifoBuffer ifb;
    unsigned char toRead [8];
    unsigned char toWrite [16];
    unsigned int nbRead;
    int (i);
    
    for (i=0; i< sizeof (toRead); i++) {
      toRead[i] = (char) (i%10)+48;
    }
    
    
    IvyFifoInit (&ifb);


    IvyFifoWrite(&ifb, toRead, 8);
    printf ("DBG> fifo size=%u, length=%u\n", IvyFifoSize(&ifb), IvyFifoLength(&ifb));

    nbRead=IvyFifoRead(&ifb, toWrite, 4);
    toWrite[nbRead] = 0;
    printf ("DBG> ndRead=%u, fifo size=%u, length=%u, buffer=%s\n", nbRead, IvyFifoSize(&ifb), 
	    IvyFifoLength(&ifb), toWrite);

    IvyFifoWrite(&ifb, toRead, 8);
    printf ("DBG> fifo size=%u, length=%u\n", IvyFifoSize(&ifb), IvyFifoLength(&ifb));

    nbRead=IvyFifoRead(&ifb, toWrite, 8);
    toWrite[nbRead] = 0;
    printf ("DBG> ndRead=%u, fifo size=%u, length=%u, buffer=%s\n", nbRead, IvyFifoSize(&ifb), 
	    IvyFifoLength(&ifb), toWrite);

   nbRead=IvyFifoRead(&ifb, toWrite, 8);
    toWrite[nbRead] = 0;
    printf ("DBG> ndRead=%u, fifo size=%u, length=%u, buffer=%s\n", nbRead, IvyFifoSize(&ifb), 
	    IvyFifoLength(&ifb), toWrite);

   nbRead=IvyFifoRead(&ifb, toWrite, 8);
    toWrite[nbRead] = 0;
    printf ("DBG> ndRead=%u, fifo size=%u, length=%u, buffer=%s\n", nbRead, IvyFifoSize(&ifb), 
	    IvyFifoLength(&ifb), toWrite);

   nbRead=IvyFifoRead(&ifb, toWrite, 4);
    toWrite[nbRead] = 0;
    printf ("DBG> ndRead=%u, fifo size=%u, length=%u, buffer=%s\n", nbRead, IvyFifoSize(&ifb), 
	    IvyFifoLength(&ifb), toWrite);

    IvyFifoWrite(&ifb, toRead, 8);
    printf ("DBG> fifo size=%u, length=%u\n", IvyFifoSize(&ifb), IvyFifoLength(&ifb));

    nbRead=IvyFifoRead(&ifb, toWrite, 8);
    toWrite[nbRead] = 0;
    printf ("DBG> ndRead=%u, fifo size=%u, length=%u, buffer=%s\n", nbRead, IvyFifoSize(&ifb), 
	    IvyFifoLength(&ifb), toWrite);

   nbRead=IvyFifoRead(&ifb, toWrite, 8);
    toWrite[nbRead] = 0;
    printf ("DBG> ndRead=%u, fifo size=%u, length=%u, buffer=%s\n", nbRead, IvyFifoSize(&ifb), 
	    IvyFifoLength(&ifb), toWrite);

   nbRead=IvyFifoRead(&ifb, toWrite, 8);
    toWrite[nbRead] = 0;
    printf ("DBG> ndRead=%u, fifo size=%u, length=%u, buffer=%s\n", nbRead, IvyFifoSize(&ifb), 
	    IvyFifoLength(&ifb), toWrite);

  }


  return (0);
}
#endif // TEST_UNITAIRE
