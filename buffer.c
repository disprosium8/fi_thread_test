#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "init.h"
#include "connect.h"
#include "buffer.h"
#include "logging.h"

int pfi_buffer_register(test_buf_t *db, void *ctx, int flags) {
  struct fid_mr *mr;
  fi_cnct_ctx *c = (fi_cnct_ctx*)ctx;
  int rc;
  
  dbg_trace("Registering buffer at addr %p of size %lu",
	    (void*)db->addr, db->size);
  
  rc = fi_mr_reg(c->dom, (void*)db->addr, db->size,
		 FI_WRITE|FI_READ|FI_REMOTE_WRITE|FI_REMOTE_READ,
		 0, 0, 0, &mr, NULL);
  if (rc) {
    dbg_err("Could not register memory at %p, size %lu: %s",
	    (void*)db->addr, db->size, fi_strerror(-rc));
    goto error_exit;
  }
  
  db->keys.key0 = fi_mr_key(mr);
  db->keys.key1 = fi_mr_key(mr);
  db->priv_ptr = (void*)mr;
  db->priv_size = sizeof(*mr);
  db->is_registered = 1;

  return TEST_OK;

error_exit:
  db->is_registered = 0;
  return TEST_ERROR;
}

int pfi_buffer_unregister(test_buf_t *db, void *ctx) {
  int rc;
  
  rc = fi_close(&((struct fid_mr*)db->priv_ptr)->fid);
  if (rc) {
    dbg_err("Could not deregister memory region: %s", fi_strerror(-rc));
    goto error_exit;
  }

  db->is_registered = 0;

  return TEST_OK;

 error_exit:
  return TEST_ERROR;
}
