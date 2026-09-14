#ifndef IVYFIFO_H
#define IVYFIFO_H

typedef struct IvyFifoBuffer {
  char *buffer;
  char *rptr, *wptr, *end;
  int  full;
} IvyFifoBuffer;


int IvyFifoInit(IvyFifoBuffer *f);

void IvyFifoFree(IvyFifoBuffer *f);

IvyFifoBuffer* IvyFifoNew (void);

void IvyFifoDelete (IvyFifoBuffer *f);

unsigned int IvyFifoLength(const IvyFifoBuffer  *f);

unsigned int IvyFifoSize(const IvyFifoBuffer  *f);

unsigned int IvyFifoAvail(const IvyFifoBuffer  *f);

unsigned int IvyFifoRead(IvyFifoBuffer *f, char *buf, unsigned int buf_size);

unsigned int IvyFifoSendSocket (IvyFifoBuffer *f, const int fd);

int IvyFifoIsFull (const IvyFifoBuffer  *f) ;

void IvyFifoWrite(IvyFifoBuffer *f, const char *buf, unsigned int size);

/* Checked variants return IvyStatus values. Writes are all-or-nothing.
 * A temporarily blocked flush is IVY_OK with a nonzero remaining length.
 */
int IvyFifoWriteChecked(IvyFifoBuffer *f, const char *buf, unsigned int size);
int IvyFifoFlush(IvyFifoBuffer *f, int fd, unsigned int *remaining, int *system_error);



#endif /* IVYFIFO_H */
