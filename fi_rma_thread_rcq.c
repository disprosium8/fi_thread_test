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
#define TVAL             13

#define MAX_SIZE         16384
#define BUF_SIZE         (1024*1024*256UL)
#define TAG              UINT32_MAX
#define SQ_SIZE          100

#define LARGE_LIMIT      16384
#define LARGE_ITERS      1000

#define SYNC_CHK         ((uint64_t)0xff<<56)
#define SYNC_REQ         ((uint64_t)0xdeadbeef<<32)
#define SYNC_REP         ((uint64_t)0xcafebabe<<32)

static int ITERS       = 100000;

static int DONE = 0;
static int myrank;
static int nranks;
static int next;
static int rthreads = 1;

static void *ctx;
static sem_t sem, rsem;

test_buf_t lbuf, rbufs[NRBUFS+1], *rds;

int do_sync() {
  int i, val;
  for (i=0; i<nranks; i++) {
    sem_wait(&rsem);
    if (i==myrank) continue;
    int n = i * (NRBUFS+1) + NRBUFS;
    pfi_rdma_put(i, lbuf.addr, rds[n].addr, 8, lbuf.priv_ptr, rds[n].keys.key0,
		 TAG+1, SYNC_REQ | myrank, TEST_FLAG_WITH_IMM);
  }
  
  // we wait for our own buffer to get cleared
  // and for the replies from peers
  do {
    if (sem_getvalue(&rsem, &val)) continue;
  } while (val < nranks);

  return 0;
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
    else if (n && (ids == TAG+1)) {
      // sync completion
    }
  } while (!DONE);
  
  pthread_exit(NULL);
}

void *wait_remote_completions_thread(void *arg) {
  long inputrank = (long)arg;
  uint64_t ids, imms;
  int i, n, rc;
  
  do {
    rc = pfi_get_revent(TEST_ANY_SOURCE, 1, &ids, &imms, &n);
    if (rc == TEST_EVENT_ERROR) {
      err(1, "Get revent error");
    }
    
    if (n && (imms>>56<<56 == SYNC_CHK)) {
      uint32_t size = (imms & ~(SYNC_CHK))>>32;
      uint32_t iter = imms<<32>>32;
      uint64_t *v = (uint64_t*)(rbufs[NRBUFS].addr + sizeof(uint64_t) * iter);
      assert(*v == TVAL);
    }
    else if (n && (imms>>32<<32 == SYNC_REQ)) {
      for (i=0; i<NRBUFS+1; i++) {
	memset((void*)rbufs[i].addr, 0, sizeof(BUF_SIZE));
      }
      // signal that we cleared our local buffers
      sem_post(&rsem);
      
      int src = imms<<32>>32;
      assert(src != myrank);
      int n = src * (NRBUFS+1) + NRBUFS;
      pfi_rdma_put(src, lbuf.addr, rds[n].addr, 8, lbuf.priv_ptr, rds[n].keys.key0,
		   TAG+1, SYNC_REP | myrank, TEST_FLAG_WITH_IMM);
    }
    else if (n && (imms>>32<<32 & SYNC_REP)) {
      // signal that we got a peer reply
      sem_post(&rsem);
    }
    else if (n && (imms == 0xfacefeed)) {
      // data put
    }
    else if (n) {
      err(1, "Got unexpected immediate data: 0x%016lx", imms);
    }
  } while (!DONE);
  
  pthread_exit(NULL);
}

int main(int argc, char **argv) {
  long t;
  int i, j, k, val;
  int rank, nproc, iters;
  pthread_t th, recv_threads[rthreads];
  
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
  
  for (i=0; i<NRBUFS+1; i++) {
    rbufs[i].addr = (uintptr_t)calloc(1, BUF_SIZE);
    rbufs[i].size = BUF_SIZE;
    pfi_buffer_register(&rbufs[i], ctx, 0);
  }
  
  rds = malloc(sizeof(rbufs)*nranks);
  MPI_Allgather(&rbufs, sizeof(rbufs), MPI_BYTE, rds, sizeof(rbufs), MPI_BYTE, MPI_COMM_WORLD);

  sem_init(&sem, 0, SQ_SIZE);
  sem_init(&rsem, 0, nranks);
  
  // Create a thread to wait for local completions
  pthread_create(&th, NULL, wait_local_completion_thread, NULL);

  for (t=0; t<rthreads; t++) {
    pthread_create(&recv_threads[t], NULL, wait_remote_completions_thread, (void*)t);
  }
  
  if (myrank == 0)
    printf("%-7s%-9s%-12s%-12s\n", "Ranks", "Senders", "Bytes", "Sync PUT");
  
  struct timespec time_s, time_e;
  for (int ns = 0; ns < nranks; ns++) {
    
    for (i=1; i<=MAX_SIZE; i+=i) {
      
      iters = (i > LARGE_LIMIT) ? LARGE_ITERS : ITERS;
      assert(iters*MAX_SIZE <= BUF_SIZE*NRBUFS);
      
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
	    } while (c<2);
	    
	    // get a remote buffer to write to
	    int n = next * (NRBUFS+1) + k % NRBUFS;
	    // compute the offset in the remote buffer for this iter
	    uintptr_t raddr = rds[n].addr + i * k;
	    // encode some immediate data so we know where to look on the target
	    uint64_t imm = SYNC_CHK | (uint64_t)i<<32 | k;
	    
	    // first part put
	    pfi_rdma_put(next, lbuf.addr, raddr, i, lbuf.priv_ptr, rds[n].keys.key0,
			 TAG, 0xfacefeed, TEST_FLAG_WITH_IMM);
	    
	    // now put to our reserved buffer
	    n = next * (NRBUFS+1) + NRBUFS;
	    raddr = rds[n].addr + sizeof(uint64_t) * k;
	    uint64_t *v = (uint64_t*)lbuf.addr;
	    *v = TVAL;
	    pfi_rdma_put(next, lbuf.addr, raddr, sizeof(uint64_t), lbuf.priv_ptr, rds[n].keys.key0,
			 TAG, imm, TEST_FLAG_WITH_IMM);
	  }
	}
      }
      
      do {
	if (sem_getvalue(&sem, &val)) continue;
      } while (val < SQ_SIZE);
      
      clock_gettime(CLOCK_MONOTONIC, &time_e);
            
      if (myrank == 0) {
	double time_ns = (double)(((time_e.tv_sec - time_s.tv_sec) * 1e9) + (time_e.tv_nsec - time_s.tv_nsec));
	double time_us = time_ns/1e3;
	double latency = time_us/iters;
	printf("%-12.2f\n", latency);
	fflush(stdout);
      }
      
      // make sure remote completions are caught up after each size
      do_sync();
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

