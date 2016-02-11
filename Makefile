CC      = gcc
CFLAGS  = $(shell pkg-config ompi,libfabric --cflags) -O3 -g -DENABLE_DEBUG
LIBS    = $(shell pkg-config ompi,libfabric --libs) -lpthread

all: fi_rma_thread fi_rma_thread_mr fi_rma_thread_rcq

fi_rma_thread: fi_rma_thread.o init.o buffer.o connect.o logging.o
	$(CC) -o $@ $^ $(LIBS)

fi_rma_thread_mr: fi_rma_thread_mr.o init.o buffer.o connect.o logging.o
	$(CC) -o $@ $^ $(LIBS)

fi_rma_thread_rcq: fi_rma_thread_rcq.o init.o buffer.o connect.o logging.o
	$(CC) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) *.o *.btr fi_rma_thread
