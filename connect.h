#ifndef CONNECT_H
#define CONNECT_H

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>

typedef struct fi_cnct_ctx_t {
  struct fi_info         *fi;
  struct fi_info         *hints;
  struct fid_fabric      *fab;
  struct fid_domain      *dom;
  struct fid_av          *av;
  struct fid_ep         **eps;
  struct fid_cq         **lcq;
  struct fid_cq         **rcq;
  struct fi_context       fi_ctx_av;
  fi_addr_t              *addrs;

  int                     thread_safe;
  void                   *local_addr;
  size_t                  addr_len;
  
  uint64_t                flags;
  char                   *eth_dev;
  char                   *node;
  char                   *service;
  char                   *domain;
  char                   *provider;

  int                     num_cq;
  int                     use_rcq;
  int                     rdma_get_align;
  int                     rdma_put_align;
} fi_cnct_ctx;

int __fi_init_context(fi_cnct_ctx *ctx);
int __fi_connect_peers(fi_cnct_ctx *ctx, struct fi_info *fi);

#endif
