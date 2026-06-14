#include <stdio.h>
#include <string.h>

#include "ivyfifo.h"

int main(void)
{
  static const char payload[] = "fifo-payload";
  char out[sizeof(payload)] = {0};
  IvyFifoBuffer *fifo = IvyFifoNew();
  unsigned int before;
  unsigned int after;
  unsigned int read_len;

  if (fifo == NULL) {
    fprintf(stderr, "IvyFifoNew failed\n");
    return 1;
  }

  IvyFifoWrite(fifo, payload, (unsigned int)strlen(payload));
  before = IvyFifoLength(fifo);
  after = IvyFifoSendSocket(fifo, -1);

  if (after != before || IvyFifoLength(fifo) != before) {
    fprintf(stderr, "IvyFifoSendSocket drained data after send failure\n");
    IvyFifoDelete(fifo);
    return 2;
  }

  read_len = IvyFifoRead(fifo, out, sizeof(out) - 1);
  if (read_len != strlen(payload) || strcmp(out, payload) != 0) {
    fprintf(stderr, "FIFO contents changed after send failure\n");
    IvyFifoDelete(fifo);
    return 3;
  }

  IvyFifoDelete(fifo);
  return 0;
}
