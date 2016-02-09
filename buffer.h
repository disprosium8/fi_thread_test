#ifndef BUFFER_H
#define BUFFER_H

typedef struct {
  uint64_t key0;
  uint64_t key1;
} test_buf_keys_t;

typedef struct {
  uintptr_t addr;
  uint64_t size;
  test_buf_keys_t keys;
  int priv_size;
  void *priv_ptr;
  uint16_t is_registered;
  uint16_t ref_count;
} test_buf_t;

int pfi_buffer_register(test_buf_t *db, void *ctx, int flags);
int pfi_buffer_unregister(test_buf_t *db, void *ctx);

#endif
