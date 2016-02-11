#include <math.h>
#include <mpi.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <err.h>

#include "init.h"
#include "buffer.h"

__attribute__((optimize("O0")))
static void dbg_wait(void) {
  int i = 0;
  char hostname[256];
  gethostname(hostname, sizeof(hostname));
  printf("PID %d on %s ready for attach\n", getpid(), hostname);
  fflush(stdout);
  while (0 == i)
    sleep(12);
}

#define NALLOC           8192
#define ALLOC_SIZE       (1024*1024*2)

#define NRBUFS           8

#define BUF_SIZE         (1024*1024*1)
#define TAG              UINT32_MAX
#define SQ_SIZE          100

#define LARGE_LIMIT      16384
#define LARGE_ITERS      1000

static int ITERS       = 100000;

static int DONE = 0;
static int myrank;
static int nranks;
static int next;
static int rthreads = 1;
static int athreads = 1;

static void *ctx;
static sem_t sem;

void *alloc_thread() {
  test_buf_t bufs[NALLOC];
  int i = 0;
  do {
    bufs[i].addr = (uintptr_t)calloc(1, ALLOC_SIZE);
    bufs[i].size = ALLOC_SIZE;
    pfi_buffer_register(&bufs[i], ctx, 0);    
    if (i >= NALLOC/2) {
      pfi_buffer_unregister(&bufs[i-NALLOC/2], ctx);
      free((void*)bufs[i-NALLOC/2].addr);
    }
    i = (++i) % NALLOC;
    usleep(1000);
  } while(!DONE);

  pthread_exit(NULL);
}

void *wait_local_completion_thread(void *arg) {
  uint64_t ids;
  int n, rc;

  do {
    n = 0;
    rc = pfi_get_event(TEST_ANY_SOURCE, 1, &ids, &n);
    if (rc == TEST_EVENT_ERROR) {
      err(1, "Get event error");
    }
    if (n && (ids == TAG)) {
      sem_post(&sem);
    }
  } while (!DONE);
  
  pthread_exit(NULL);
}

void *wait_remote_completions_thread(void *arg) {
  long inputrank = (long)arg;
  uint64_t ids, imms;
  int n, rc;
  
  do {
    rc = pfi_get_revent(TEST_ANY_SOURCE, 1, &ids, &imms, &n);
    if (rc == TEST_EVENT_ERROR) {
      err(1, "Get revent error");
    }
    if (n) {
      err(1, "Not expecting a remote event!");
    }
  } while (!DONE);
  
  pthread_exit(NULL);
}

int main(int argc, char **argv) {
  long t;
  int i, j, k, val;
  int rank, nproc, iters;
  pthread_t th, recv_threads[rthreads], th2[athreads];
  test_buf_t lbuf, rbufs[NRBUFS], *rds;
  
  MPI_Init(&argc,&argv);

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nproc);

  myrank = rank;
  nranks = nproc;
  next = (myrank+1) % nranks;
  
  ctx = pfi_init(rank, nproc);
  if (!ctx) {
    err(1, "Could not initialize libfabric");
  }

  if (rthreads == 0)
    rthreads = nproc;
  
  lbuf.addr = (uintptr_t)malloc(BUF_SIZE);
  lbuf.size = BUF_SIZE;
  pfi_buffer_register(&lbuf, ctx, 0);
  
  for (i=0; i<NRBUFS; i++) {
    rbufs[i].addr = (uintptr_t)malloc(BUF_SIZE);
    rbufs[i].size = BUF_SIZE;
    pfi_buffer_register(&rbufs[i], ctx, 0);
  }

  rds = malloc(sizeof(rbufs)*nranks);
  MPI_Allgather(&rbufs, sizeof(rbufs), MPI_BYTE, rds, sizeof(rbufs), MPI_BYTE, MPI_COMM_WORLD);

  sem_init(&sem, 0, SQ_SIZE);

  // Create a thread to wait for local completions
  pthread_create(&th, NULL, wait_local_completion_thread, NULL);

  for (t=0; t<rthreads; t++) {
    pthread_create(&recv_threads[t], NULL, wait_remote_completions_thread, (void*)t);
  }
  
  // Create some threads that aggressively allocate and register memory
  for (t=0; t<athreads; t++) {
    pthread_create(&th2[t], NULL, alloc_thread, NULL);
  }

  if (myrank == 0)
    printf("%-7s%-9s%-12s%-12s\n", "Ranks", "Senders", "Bytes", "Sync GET");
  
  struct timespec time_s, time_e;
  for (int ns = 1; ns < nranks; ns++) {
    
    for (i=1; i<=BUF_SIZE; i+=i) {
      
      iters = (i > LARGE_LIMIT) ? LARGE_ITERS : ITERS;
      
      if (myrank == 0) {
	printf("%-7d", nranks);
	printf("%-9u", ns + 1);
	printf("%-12u", i);
	fflush(stdout);
      }
      
      if (myrank <= ns) {
	clock_gettime(CLOCK_MONOTONIC, &time_s);
	for (k=0; k<iters; k++) {
	  if (sem_wait(&sem) == 0) {
	    int c;
	    do {
	      c = pfi_tx_size_left(next);
	    } while (!c);
	    int n = next * NRBUFS + k % NRBUFS;
	    pfi_rdma_get(next, lbuf.addr, rds[n].addr, i, lbuf.priv_ptr, rds[n].keys.key0, TAG, 0);
	    //pfi_rdma_put(next, lbuf.addr, rds[n].addr, i, lbuf.priv_ptr, rds[n].keys.key0, TAG, 0, 0);
	  }
	}
      }
      
      do {
	if (sem_getvalue(&sem, &val)) continue;
      } while (val < SQ_SIZE);
      
      clock_gettime(CLOCK_MONOTONIC, &time_e);
      
      MPI_Barrier(MPI_COMM_WORLD);
      
      if (myrank == 0) {
	double time_ns = (double)(((time_e.tv_sec - time_s.tv_sec) * 1e9) + (time_e.tv_nsec - time_s.tv_nsec));
	double time_us = time_ns/1e3;
	double latency = time_us/iters;
	printf("%-12.2f\n", latency);
	fflush(stdout);
      }
    }
  }  

  MPI_Barrier(MPI_COMM_WORLD);
  
  DONE = 1;
  
  // Wait for all threads to join
  pthread_join(th, NULL);
  for (t=0; t<rthreads; t++) {
    pthread_join(recv_threads[t], NULL);
  }

  pfi_buffer_unregister(&lbuf, ctx);
  free((void*)lbuf.addr);

  for (i=0; i<NRBUFS; i++) {
    pfi_buffer_unregister(&rbufs[i], ctx);
    free((void*)rbufs[i].addr);
  }
  
  pfi_finalize();
  
  MPI_Finalize();

  return 0;
}

