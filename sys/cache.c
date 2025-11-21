/*! @file cache.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‌‌​‌‍‍⁠​⁠⁠‌
    @brief Block cache for a storage device.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef CACHE_TRACE
#define TRACE
#endif

#ifdef CACHE_DEBUG
#define DEBUG
#endif

#include "cache.h"

#include "conf.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"

#define FLUSH_IN_PROGRESS 1
#define FLUSH_NOT_IN_PROGRESS 0

// INTERNAL TYPE DEFINITIONS
//
struct cache {
    int per_block_pos[CACHE_SIZE]; //NOTE: these field was changed to uint64_t from uint8 for obvious reasons// ok why the hell won't uint8_t compile here
    int per_block_last_accessed[CACHE_SIZE];
    struct lock per_block_locks[CACHE_SIZE];// indexes for cache_blocks correspond to indexes for cache_locks
    struct storage* disk;

    //struct condition flushed; //blocks while the cache is flushing, so that no threads can acquire any locks, but an still have a none adversarial position within the cache-modifying functions
    //int flush_in_progress; //a "signal that goes high when the flush begins, so that any threads that try to get to acquring locks get stuck waiting on the above function
                            //and EVENTUALLY RETURN in such a way that they were going to do something but never did it.
    
}; 

struct cache_block{char data[512];};

static struct cache_block cache_block_raw[CACHE_SIZE]; //a raw array of storage_fetched blocks to replace all the kmallocing.
                                                       //for example, our cache_block:0, fetched at pos 512, will be placed at 0, and inside the cache instance, the per_block_pos[0] gets changed to 512

/**
 * @brief finds the least recently used block on the list that ISN"T currently locked
 * @param cptr Pointer to the cache.
 * @return index of the lru+unlocked block on success, negative error code if error
 */
int find_lru_unlocked(struct cache * cache){

    //reminder: higher last_accessed means more recently used
    
    //algorithm: we do the same thing as we were doing before, but this time, moving on to the next least recently used block if the first least recently used is locked
    //          designed to behave just like the previous one in the case of one thread, and slightly different in the case of multiple threads

    //4294967295 ru sure about this?
    int lru_locked = 4294967295; //keeps the last_accessed value of an lru if the lru is locked. or if the second lru is also locked... or if the third lru is also also locked

    int num_lru_locked = 0;
    while(num_lru_locked < CACHE_SIZE){ 
        int lru = -1;
        int last_access_greatest = -1; //variable to store the greatest last access of each iteration of the inner loop

        for (int i = 0; i < CACHE_SIZE; i++){ //the function of this inner loop is to find an lru candidate
            if (!(cache->per_block_last_accessed[i] > last_access_greatest)) continue;         
            if (!(cache->per_block_last_accessed[i] < lru_locked)) continue;
            lru = i;
            last_access_greatest = cache->per_block_last_accessed[i];
        }

        if (!cache->per_block_locks[lru].owner) return lru; //at this point if the cache_block hasn't been locked down, just take it for eviction

        lru_locked = cache->per_block_last_accessed[lru];
        num_lru_locked++; //if the if statement didn't go through, increment the number of lrus we've gone through that have been locked
    }
    return 0;//if all 64 blocks are locked we've reached undefined behavior I guess I'll just return 0 so it can try to evict that

}

/**
 * @brief Creates/initializes a cache with the passed backing storage device (disk) and makes it
 * available through cptr.
 * @param disk Pointer to the backing storage device.
 * @param cptr Pointer to the cache to create.
 * @return 0 on success, negative error code if error
 */
int create_cache(struct storage* disk, struct cache** cptr) {
    // FIXME
    trace("%s(storage=%p,cptr=%p)", __func__, disk, cptr);
    if(!disk || !cptr) return -EINVAL;// if either argument is null, return immediately

    struct cache * c = kcalloc(1, sizeof(struct cache));
    if(!c) return -EINVAL;// if c is NULL then kcalloc failed, so return error code

    for (int i = 0; i < CACHE_SIZE; i++){
        //c->blocks[i] = NULL; no need to do this anymore. 
        c->per_block_pos[i] = -1; //-1 is used explicitly to say that the "block slot" in the cache hasn't been taken up yet
        lock_init(&c->per_block_locks[i]);
    }

    c->disk = disk;
    *cptr = c;
 
    // c->flush_in_progress = FLUSH_NOT_IN_PROGRESS;
    // condition_init(&c->flushed, "cache_flushed");
    return 0;
    
}

/**
 * @brief Reads a CACHE_BLKSZ sized block from the backing interface into the cache.
 * @param cache Pointer to the cache.
 * @param pos Position in the backing storage device. Must be aligned to a multiple of the block
 * size of the backing interface.
 * @param pptr Pointer to the block pointer read from the cache. Assume that CACHE_BLKSZ will always
 * be equal to the block size of the storage disk. Any replacement policy is permitted, as long as
 * your design meets the above specifications.
 * @return 0 on success, negative error code if error
 */
int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr) {
    trace("%s(cache=%p, pos=%u,pptr=%p)\n", __func__, cache,pos, pptr);
    int retval;

    // while (cache->flush_in_progress) {
    //     condition_wait(&cache->flushed);
    // } //eventually resumes normally. we're just holding this guy because we don't want him to acquire any locks

    if (pos % CACHE_BLKSZ) return -EINVAL;
    trace("address of cache_block_raw:%p\n",cache_block_raw);

    for (int i = 0; i < CACHE_SIZE; i++){
        //if (cache->per_block_pos[i] != -1 && cache->per_block_pos[i] == pos){
        if (cache->per_block_pos[i] == pos){ //don't need to check for -1... pos can only be positive
            trace("block found in cache at index %d\n", i);
            trace("lock acquired for block %d\n", i);
            lock_acquire(&cache->per_block_locks[i]);

            *pptr = &cache_block_raw[i].data;
            return 0;
        }
    }

    int avail_slot = 0;
    while (avail_slot < CACHE_SIZE){
        if (cache->per_block_pos[avail_slot] == -1){
            break;
        }
        avail_slot++;
    }

    if (avail_slot < CACHE_SIZE){
        trace("there was a slot available on the cache table at index %d\n", avail_slot);
        trace("lock acquired for block %d\n", avail_slot);
        lock_acquire(&cache->per_block_locks[avail_slot]);

        //cache->blocks[avail_slot] = kcalloc(1, sizeof(struct cache_block));//NOT KCALLOCING ANYMORE!!!
        cache->per_block_pos[avail_slot] = pos;
        
        retval = storage_fetch(cache->disk, pos, &cache_block_raw[avail_slot], CACHE_BLKSZ);
        // trace("storage_fetch returned %d\n", retval);
        // trace("first bytes: %02x %02x %02x %02x\n", 
        // cache_block_raw[avail_slot].data[0],
        // cache_block_raw[avail_slot].data[1], 
        // cache_block_raw[avail_slot].data[2],
        // cache_block_raw[avail_slot].data[3]);
        if (retval<0) {
            trace("storage_fetch failed \n");
            lock_release(&cache->per_block_locks[avail_slot]); //forgot to release this lock in prior commits
            return retval;
        }

        *pptr = &cache_block_raw[avail_slot];
        trace("blockptr = %p\n", *pptr);
        return 0;
    }
    

    trace("there were no slots available. evict block \n");

    //lru search: this loop searches for the least recently used block and stores its index in lru
    int lru = -1;
    int lru_max = -1;
    for (int i = 0; i < CACHE_SIZE; i++){        
        if (!(cache->per_block_last_accessed[i] > lru_max)) continue;

        lru = i;
        lru_max = cache->per_block_last_accessed[i];
    }

    //int lru = find_lru_unlocked(cache);
    
    trace("lock acquired for block %d\n", avail_slot);
    lock_acquire(&cache->per_block_locks[lru]); //this has to come after the lru search. because we need the lru index

    
    retval = storage_fetch(cache->disk, pos, &cache_block_raw[lru], CACHE_BLKSZ);
    if (retval<0) {
        trace("storage_fetch failed \n");
        lock_release(&cache->per_block_locks[avail_slot]); //forgot to release this lock in prior commits
        return retval;
    }

    cache->per_block_pos[lru] = pos;
    *pptr = &cache_block_raw[lru];
    return 0;
}

/**
 * @brief Releases a block previously obtained from cache_get_block().
 * @param cache Pointer to the cache.
 * @param pblk Pointer to a block that was made available in cache_get_block() (which means that
 * pblk == *pptr for some pptr).
 * @param dirty Indicates whether the block has been modified (1) or not (0). If dirty == 1, the
 * block has been written to. If dirty == 0, the block has not been written to.
 * @return 0 on success, negative error code if error
 */
void cache_release_block(struct cache* cache, void* pblk, int dirty) {
    trace("%s(cache=%p,pblk=%p, dirty=%d)", __func__, cache, pblk, dirty);

    // while (cache->flush_in_progress) {
    //     condition_wait(&cache->flushed);
    // } //eventually resumes normally. we're just holding this guy because we don't want him to acquire any locks
      
      
    //struct cache_block * block = (void *)pblk - offsetof(struct cache_block, data);//pretty bad honestly
    

    //finds finds the index of the block we'd like to release
    // int curr_block_index = 0;
    // for (int i=0; i < CACHE_SIZE; i++){
    //     if ((struct cache_block *)&cache_block_raw + i== (struct cache_block *)&pblk){
    //         curr_block_index = i;
    //         break;
    //     }
    // }
    //int curr_block_index = (int)((char)cache_block_raw - pblk)/CACHE_BLKSZ;
    //size_t curr_block_index = cache_block_raw - (struct cache_block *)pblk; //cache_block_raw decays to its type;
    int curr_block_index = (struct cache_block *) pblk - cache_block_raw;
    trace("curr_block_index: %d\n", curr_block_index);
    
    lock_acquire(&cache->per_block_locks[curr_block_index]);

    if (dirty ==1){ //store here
        int retval = storage_store(cache->disk, cache->per_block_pos[curr_block_index], pblk, CACHE_BLKSZ);
        if (retval < 0) return;//I still cannot just allow the block to be unlocked. just return and thats enough. not that this was NOT a change
    }


    trace("updateing last access for block index %d\n", curr_block_index);
    int blockindex = 0;
    while(blockindex < CACHE_SIZE)
    {
        if(cache->per_block_last_accessed[blockindex] != -1 && curr_block_index != blockindex)
        {
            cache->per_block_last_accessed[blockindex]++;
        }
        blockindex++;
    }
    cache->per_block_last_accessed[curr_block_index]= 0;

    lock_release(&cache->per_block_locks[curr_block_index]);
    //trace("between the first and second release\n");
    lock_release(&cache->per_block_locks[curr_block_index]);
 
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {

    for (int i = 0; i < CACHE_SIZE; i++){
        lock_acquire(&cache->per_block_locks[i]);
        lock_release(&cache->per_block_locks[i]);
    }

    // while (cache->flush_in_progress) { //imagine trying to throw a wrench in here lol
    //     condition_wait(&cache->flushed);
    // } //eventually resumes normally. we're just holding this guy because we don't want him to acquire any locks
    // //or should this be an early return with a negative value????????
    //
    // cache->flush_in_progress = FLUSH_IN_PROGRESS;
    //
    // for (int i = 0; i < CACHE_SIZE; i++){
    //     lock_acquire(&cache->cache_locks[i]);
    // }
    // for (int i = 0; i < CACHE_SIZE; i++){
    //     lock_release(&cache->cache_locks[CACHE_SIZE - i]);
    // }
    //
    // cache->flush_in_progress = FLUSH_NOT_IN_PROGRESS;
    // condition_broadcast(&cache->flushed);
    return 0;
}
