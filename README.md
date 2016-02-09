# fi_thread_test
Testing libfabric distributed and multi-threading

Simple distributed MPI test, spawning threads for local and remote completion.
Using a semaphore to sync between posted operations and completions, so there
will be lots of contention on a single node.

Test seems stable with a single sender, but there is a reproducible hang when starting with 2 senders.

Example run (sockets, eth0):

```
$ mpirun -np 2 -map-by node:PE=16 ./fi_rma_thread
ALL:DBG: 0 (pfi_init:88): > Found matching ethernet device: eth0 (192.168.1.2)
ALL:DBG: 1 (pfi_init:88): > Found matching ethernet device: eth0 (192.168.1.3)
ALL:DBG: 0 (__fi_init_context:172): > Created FI domain on sockets : IP : FI_EP_RDM
ALL:DBG: 1 (__fi_init_context:172): > Created FI domain on sockets : IP : FI_EP_RDM
Ranks  Senders  Bytes       Sync GET                
2      2        1           15.43       
2      2        2           9.27        
2      2        4           9.42        
2      2        8           9.36        
2      2        16          9.40        
2      2        32          9.39        
2      2        64          9.41        
2      2        128         9.62        
2      2        256         9.82        
2      2        512         10.66       
2      2        1024        10.86       
2      2        2048        11.80       
2      2        4096        12.52       
2      2        8192        13.93       
2      2        16384       17.48       
2      2        32768       32.32       
2      2        65536       216.34      
2      2        131072      149.54      
2      2        262144     
```
