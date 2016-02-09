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

static void __attribute__((used)) dbg_wait(void) {
  int i = 0;
  char hostname[256];
  gethostname(hostname, sizeof(hostname));
  printf("PID %d on %s ready for attach\n", getpid(), hostname);
  fflush(stdout);
  while (0 == i)
    sleep(12);
}

#if defined(linux)
#define HAVE_SETAFFINITY
#include <sched.h>
#endif

#ifndef CPU_SETSIZE
#undef HAVE_SETAFFINITY
#endif

#define BUF_SIZE         (1024*1024*1)
#define TAG              UINT32_MAX
#define SQ_SIZE          1000

#define LARGE_LIMIT      16384
#define LARGE_ITERS      1000

static int ITERS       = 10000;
static char *sbuf, *rbuf;

static int DONE = 0;
static int myrank;
static int nranks;
static int rthreads = 1;

static sem_t sem;

#ifdef HAVE_SETAFFINITY
static int ncores = 1;
static cpu_set_t cpu_set;
static cpu_set_t def_set;
#endif

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
  int rank, nproc, iters, next;
  int aff_main, aff_evq, aff_ledg;
  pthread_t th, recv_threads[rthreads];
  test_buf_t ld, rd, *rds;
  
  MPI_Init(&argc,&argv);

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nproc);

  myrank = rank;
  nranks = nproc;
  next = (myrank+1) % nranks;
  
  void *ctx = pfi_init(rank, nproc);
  if (!ctx) {
    err(1, "Could not initialize libfabric");
  }

  if (rthreads == 0)
    rthreads = nproc;
  
  aff_main = aff_evq = aff_ledg = -1;
  if (argc > 1)
    aff_main = atoi(argv[1]);
  if (argc > 2)
    aff_evq = atoi(argv[2]);
  if (argc > 3)
    aff_ledg = atoi(argv[3]);

  sbuf = malloc(BUF_SIZE);
  ld.addr = (uintptr_t)sbuf;
  ld.size = BUF_SIZE;
  pfi_buffer_register(&ld, ctx, 0);
  
  rbuf = malloc(BUF_SIZE);
  rd.addr = (uintptr_t)rbuf;
  rd.size = BUF_SIZE;
  pfi_buffer_register(&rd, ctx, 0);

  rds = malloc(sizeof(rd)*nranks);
  MPI_Allgather(&rd, sizeof(rd), MPI_BYTE, rds, sizeof(rd), MPI_BYTE, MPI_COMM_WORLD);

  sem_init(&sem, 0, SQ_SIZE);

  // Create a thread to wait for local completions 
  pthread_create(&th, NULL, wait_local_completion_thread, NULL);

  for (t=0; t<rthreads; t++) {
    pthread_create(&recv_threads[t], NULL, wait_remote_completions_thread, (void*)t);
  }
  
  // set affinity as requested
#ifdef HAVE_SETAFFINITY
  if ((ncores = sysconf(_SC_NPROCESSORS_CONF)) <= 0)
    err(1, "sysconf: couldn't get _SC_NPROCESSORS_CONF");
  CPU_ZERO(&def_set);
  for (i=0; i<ncores; i++)
    CPU_SET(i, &def_set);
  if (aff_main >= 0) {
    CPU_ZERO(&cpu_set);
    CPU_SET(aff_main, &cpu_set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set) != 0)
      err(1, "couldn't change CPU affinity");
  }
  else
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &def_set);
  if (aff_evq >= 0) {
    CPU_ZERO(&cpu_set);
    CPU_SET(aff_evq, &cpu_set);
    pthread_setaffinity_np(th, sizeof(cpu_set_t), &cpu_set);
  }
  else
    pthread_setaffinity_np(th, sizeof(cpu_set_t), &def_set);
  if (aff_evq >= 0) {
    CPU_ZERO(&cpu_set);
    CPU_SET(aff_ledg, &cpu_set);
    for (i=0; i<rthreads; i++)
      pthread_setaffinity_np(recv_threads[i], sizeof(cpu_set_t), &cpu_set);
  }
  else {
    for (i=0; i<rthreads; i++)
      pthread_setaffinity_np(recv_threads[i], sizeof(cpu_set_t), &def_set);
  }
#endif

  if (myrank == 0)
    printf("%-7s%-9s%-12s%-12s%-12s\n", "Ranks", "Senders", "Bytes", "Sync GET");
  
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
	    pfi_rdma_get(next, ld.addr, rds[next].addr, i, ld.priv_ptr, rds[next].keys.key0, TAG, 0);
	    //pfi_rdma_put(next, ld.addr, rds[next].addr, i, ld.priv_ptr, rds[next].keys.key0, TAG, 0, 0);
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

  pfi_buffer_unregister(&ld, ctx);
  free(sbuf);
  pfi_buffer_unregister(&rd, ctx);
  free(rbuf);
  
  pfi_finalize();
  
  MPI_Finalize();

  return 0;
}

