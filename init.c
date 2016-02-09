#define _GNU_SOURCE
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <limits.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

#include <rdma/fi_rma.h>

#include "init.h"
#include "buffer.h"
#include "connect.h"
#include "logging.h"

#define MAX_RETRIES 1

int _test_myrank;
int _test_nproc;

//static tatas_lock_t op_lock;
static int __initialized = 0;

static fi_cnct_ctx fi_ctx = {
  .thread_safe = 0,
  .node = NULL,
  .service = NULL,
  .domain = NULL,
  .provider = NULL,
  .num_cq = 1,
  .rdma_put_align = TEST_FI_PUT_ALIGN,
  .rdma_get_align = TEST_FI_GET_ALIGN
};

static void cq_readerr(struct fid_cq *cq, char *cq_str)
{ 
  struct fi_cq_err_entry cq_err;
  const char *err_str;
  int ret;
  
  ret = fi_cq_readerr(cq, &cq_err, 0);
  if (ret < 0) {
    dbg_err("Could not read error from CQ");
  } else {
    err_str = fi_cq_strerror(cq, cq_err.prov_errno, cq_err.err_data, NULL, 0);
    log_err("%s: %d %s", cq_str, cq_err.err, fi_strerror(cq_err.err));
    log_err("%s: prov_err: %s (%d)", cq_str, err_str, cq_err.prov_errno);
  }
}

void *pfi_init(int rank, int nproc) {
  
  __initialized = -1;
  _test_myrank = rank;
  _test_nproc  = nproc;

  fi_ctx.num_cq  = 1;
  fi_ctx.use_rcq = 1;
  fi_ctx.eth_dev = "eth0";
  fi_ctx.node    = NULL;
  fi_ctx.service = NULL;
  fi_ctx.domain  = NULL;
  fi_ctx.provider = "sockets";

  if (fi_ctx.eth_dev) {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
      dbg_err("Cannot get interface addrs");
      goto error_exit;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == NULL)
	continue;
      
      if (!strcmp(ifa->ifa_name, fi_ctx.eth_dev) &&
	  ifa->ifa_addr->sa_family == AF_INET) {
	
	fi_ctx.node = inet_ntoa(((struct sockaddr_in*)(ifa->ifa_addr))->sin_addr);
	dbg_info("Found matching ethernet device: %s (%s)",
		 ifa->ifa_name, fi_ctx.node);
	break;
      }
    }
  }

  fi_ctx.hints = fi_allocinfo();
  if (!fi_ctx.hints) {
    log_err("Could not allocate space for fi hints");
    goto error_exit;
  }

  fi_ctx.hints->domain_attr->name = strdup(fi_ctx.provider);
  fi_ctx.hints->domain_attr->mr_mode = FI_MR_BASIC;
  fi_ctx.hints->ep_attr->type = FI_EP_RDM;
  fi_ctx.hints->caps = FI_MSG | FI_RMA | FI_RMA_EVENT;
  fi_ctx.hints->mode = FI_CONTEXT | FI_LOCAL_MR | FI_RX_CQ_DATA;

  if(__fi_init_context(&fi_ctx)) {
    log_err("Could not initialize libfabric context");
    goto error_exit;
  }
  
  fi_freeinfo(fi_ctx.hints);
  
  __initialized = 1;
  
  return &fi_ctx;

error_exit:
  return NULL;
}

int pfi_finalize() {
  return TEST_OK;
}

int pfi_rdma_put(int proc, uintptr_t laddr, uintptr_t raddr, uint64_t size,
		 void *ldesc, uint64_t rkey, uint64_t id,
		 uint64_t imm_data, int flags) {
  
  uint64_t *lid = malloc(sizeof(uint64_t));
  int rc;

  memcpy(lid, &id, sizeof(*lid));

  //(!fi_ctx.thread_safe) ? sync_tatas_acquire(&op_lock):NULL;
  {  
    if (fi_ctx.use_rcq && (flags & TEST_FLAG_WITH_IMM)) {
      rc = fi_writedata(fi_ctx.eps[_test_myrank], (void*)laddr, size,
			fi_mr_desc(ldesc), imm_data, fi_ctx.addrs[proc],
			raddr, rkey, lid);
    }
    else {
      rc = fi_write(fi_ctx.eps[_test_myrank], (void*)laddr, size,
		    fi_mr_desc(ldesc), fi_ctx.addrs[proc], raddr,
		    rkey, lid);
    }
    if (rc) {
      log_err("Could not PUT to %p, size %lu: %s", (void*)raddr, size,
	      fi_strerror(-rc));
      goto error_exit;
    }
  }
  //(!fi_ctx.thread_safe) ? sync_tatas_release(&op_lock):NULL;
  
  return TEST_OK;
  
 error_exit:
  free(lid);
  return TEST_ERROR;
}

int pfi_rdma_get(int proc, uintptr_t laddr, uintptr_t raddr, uint64_t size,
		 void *ldesc, uint64_t rkey, uint64_t id, int flags) {
  
  uint64_t *lid = malloc(sizeof(uint64_t));
  int rc;

  memcpy(lid, &id, sizeof(*lid));
  
  //(!fi_ctx.thread_safe) ? sync_tatas_acquire(&op_lock):NULL;
  {  
    rc = fi_read(fi_ctx.eps[_test_myrank], (void*)laddr, size,
		 fi_mr_desc(ldesc), fi_ctx.addrs[proc], raddr,
		 rkey, lid);
    if (rc) {
      log_err("Could not GET from %p, size %lu: %s", (void*)raddr, size,
	      fi_strerror(-rc));
      goto error_exit;
    }
  }
  //(!fi_ctx.thread_safe) ? sync_tatas_release(&op_lock):NULL;

  return TEST_OK;
  
 error_exit:
  free(lid);
  return TEST_ERROR;
}

int pfi_tx_size_left(int proc) {
  int c;
  //(!fi_ctx.thread_safe) ? sync_tatas_acquire(&op_lock):NULL;
  {
    c = (int)fi_tx_size_left(fi_ctx.eps[_test_myrank]);
  }
  //(!fi_ctx.thread_safe) ? sync_tatas_release(&op_lock):NULL;
  return c;
}

int pfi_rx_size_left(int proc) {
  int c;
  //(!fi_ctx.thread_safe) ? sync_tatas_acquire(&op_lock):NULL;
  {
    c = (int)fi_rx_size_left(fi_ctx.eps[_test_myrank]);
  }
  //(!fi_ctx.thread_safe) ? sync_tatas_release(&op_lock):NULL;
  return c;
}

int pfi_get_event(int proc, int max, uint64_t *ids, int *n) {
  int i, j, ne, comp;
  int start, end;
  int retries;
  struct fi_cq_data_entry entries[MAX_CQ_POLL];
  
  *n = 0;
  comp = 0;

  if (fi_ctx.num_cq == 1) {
    start = 0;
    end = 1;
  }
  else if (proc == TEST_ANY_SOURCE) {
    start = 0;
    end = fi_ctx.num_cq;
  }
  else {
    start = TEST_GET_CQ_IND(fi_ctx.num_cq, proc);
    end = start+1;
  }

  //(!fi_ctx.thread_safe) ? sync_tatas_acquire(&op_lock):NULL;
  {
    for (i=start; i<end && comp<max; i++) {
      retries = MAX_RETRIES;
      do {
	ne = fi_cq_read(fi_ctx.lcq[i], entries, max);
	if (ne < 0) {
	  if (ne == -FI_EAGAIN) {
	    ne = 0;
	    continue;
	  }
	  else if (ne == -FI_EAVAIL) {
	    cq_readerr(fi_ctx.lcq[i], "local CQ");
	    goto error_exit;
	  }
	  else {
	    log_err("fi_cq_read() failed: %s", fi_strerror(-ne));
	    goto error_exit;
	  }
	}
      }
      while ((ne < 1) && --retries);
      
      for (j=0; j<ne && j<MAX_CQ_POLL; j++) {
	ids[j+comp] = *((uint64_t*)entries[j].op_context);
	free(entries[j].op_context);
      }
      comp += ne;
    }
    
    *n = comp;
  }
  //(!fi_ctx.thread_safe) ? sync_tatas_release(&op_lock):NULL;

  // CQs are empty
  if (comp == 0) {
    return TEST_EVENT_NONE;
  }
  
  return TEST_EVENT_OK;
  
error_exit:
  return TEST_EVENT_ERROR;
}

int pfi_get_revent(int proc, int max, uint64_t *ids, uint64_t *imms, int *n) {
  int i, j, ne, comp;
  int start, end;
  int retries;
  struct fi_cq_data_entry entries[MAX_CQ_POLL];

  if (!fi_ctx.use_rcq) {
    return TEST_EVENT_NOTIMPL;
  }
  
  *n = 0;
  comp = 0;

  if (fi_ctx.num_cq == 1) {
    start = 0;
    end = 1;
  }
  else if (proc == TEST_ANY_SOURCE) {
    start = 0;
    end = fi_ctx.num_cq;
  }
  else {
    start = TEST_GET_CQ_IND(fi_ctx.num_cq, proc);
    end = start+1;
  }

  //(!fi_ctx.thread_safe) ? sync_tatas_acquire(&op_lock):NULL;
  {
    for (i=start; i<end && comp<max; i++) {
      retries = MAX_RETRIES;
      do {
	ne = fi_cq_read(fi_ctx.rcq[i], entries, max);
	if (ne < 0) {
	  if (ne == -FI_EAGAIN) {
	    ne = 0;
	    continue;
	  }
	  else if (ne == -FI_EAVAIL) {
	    cq_readerr(fi_ctx.rcq[i], "remote CQ");
	    goto error_exit;
	  }
	  else {
	    log_err("fi_cq_read() failed: %s", fi_strerror(-ne));
	    goto error_exit;
	  }
	}
      }
      while ((ne < 1) && --retries);

      for (j=0; j<ne && j<MAX_CQ_POLL; j++) {
	// there are no revent IDs associated with this context
	ids[j+comp] = 0x0;
	imms[j+comp] = entries[j].data;
      }
      comp += ne;
    }
    *n = comp;
  }
  //(!fi_ctx.thread_safe) ? sync_tatas_release(&op_lock):NULL;
  
  // CQs are empty
  if (comp == 0) {
    return TEST_EVENT_NONE;
  }

  return TEST_EVENT_OK;

 error_exit:
  return TEST_EVENT_ERROR;
}
