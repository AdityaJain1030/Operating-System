/*! @file cache.h‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‌‌​‌‍‍⁠​⁠⁠‌
    @brief Block cache for a storage device.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/
#include "thread.h"

#ifndef _CACHE_H_
#define _CACHE_H_

#define CACHE_BLKSZ 512  // size of cache block
#define CACHE_SIZE 64

struct storage;  // external
struct cache;    // opaque decl.
// struct cache {
//     struct cache_block * blocks[CACHE_SIZE];
//     struct lock cache_locks[CACHE_SIZE];// indexes for cache_blocks correspond to indexes for cache_locks
//     struct storage* disk;

// };// __attribute__((packed));
// struct cache_block { // Could this cause some problems since cache_block size is not equal to CACHE_BLKSZ?
//     char data[CACHE_BLKSZ];
//     int block_pos; //change to block index
//     int last_access;
// }; //__attribute__((packed));
// struct cache {
//     struct cache_block * blocks[CACHE_SIZE];
//     struct lock cache_locks[CACHE_SIZE];// indexes for cache_blocks correspond to indexes for cache_locks
//     struct storage* disk;
//     struct lock lru_lock;

// }; //__attribute__((packed));


extern int create_cache(struct storage* sto, struct cache** cptr);
extern int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr);
extern void cache_release_block(struct cache* cache, void* pblk, int dirty);
extern int cache_flush(struct cache* cache);

#endif  // _CACHE_H_
