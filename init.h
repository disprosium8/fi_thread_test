#ifndef INIT_FI_H
#define INIT_FI_H

#include <mpi.h>

#include <stdint.h>
#include <rdma/fabric.h>

#define TEST_OK            0
#define TEST_ERROR         1

#define TEST_ANY_SOURCE   -1

enum {
  TEST_EVENT_OK = 0,
  TEST_EVENT_ERROR,
  TEST_EVENT_NONE,
  TEST_EVENT_NOTIMPL
};

#define TEST_FI_PUT_ALIGN  1
#define TEST_FI_GET_ALIGN  1

#define TEST_FLAG_NIL      0
#define TEST_FLAG_WITH_IMM 1

#define TEST_GET_CQ_IND(n, i) ((n > 1) ? (i % n) : 0)


#ifndef FT_FIVERSION
#define FT_FIVERSION FI_VERSION(FI_MAJOR_VERSION,FI_MINOR_VERSION)
#endif

// for RMA ops we want to be able to select fi_writedata, but there is no
// constant in libfabric for this
enum ft_rma_opcodes {
	FT_RMA_READ = 1,
	FT_RMA_WRITE,
	FT_RMA_WRITEDATA,
};

#define MAX_CQ_POLL    8

extern int _test_myrank;
extern int _test_nproc;

int pfi_initialized(void);
void *pfi_init(int rank, int nproc);
int pfi_finalize(void);
int pfi_rdma_put(int proc, uintptr_t laddr, uintptr_t raddr, uint64_t size,
		 void *ldesc, uint64_t rkey, uint64_t id, uint64_t imm_data,
		 int flags);
int pfi_rdma_get(int proc, uintptr_t laddr, uintptr_t raddr, uint64_t size,
		 void *ldesc, uint64_t rkey, uint64_t id, int flags);
int pfi_tx_size_left(int proc);
int pfi_rx_size_left(int proc);
int pfi_get_event(int proc, int max, uint64_t *ids, int *n);
int pfi_get_revent(int proc, int max, uint64_t *ids, uint64_t *imms, int *n);

#endif
