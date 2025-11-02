/*! @file cache.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌‍‌⁠​⁠⁠‌
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

// INTERNAL TYPE DEFINITIONS
//

// #define DEBUG_CACHE // USE THIS WHEN YOU NEED A SIMPLE WRITE THROUGH CACHE

/**
 * @brief Cache block structure
 */
struct cache_block {
    unsigned long long pos;        ///< Position in backing storage
    void* data;                    ///< Block data
    int dirty;                     ///< Dirty flag (1 if modified, 0 if clean)
    int valid;                     ///< Valid flag (1 if contains valid data, 0 if empty)
    struct cache_block* next;     ///< Next block in LRU list
    struct cache_block* prev;     ///< Previous block in LRU list
    int refcnt;                    ///< Number of active users of this block
    int loading;                   ///< 1 if a thread is loading/writing this block
};

/**
 * @brief Cache structure
 */
struct cache {
    struct storage* disk;          ///< Backing storage device
    struct cache_block* blocks;   ///< Array of cache blocks
    struct cache_block* lru_head; ///< Head of LRU list (most recently used)
    struct cache_block* lru_tail; ///< Tail of LRU list (least recently used)
    int capacity;                  ///< Cache capacity
    int used;                      ///< Number of used blocks
    struct lock lock;              ///< Protects cache metadata
    struct condition cond;        ///< Condition variable for wait/wake
};

// INTERNAL FUNCTION DECLARATIONS
//
#ifndef DEBUG_CACHE
static void lru_remove(struct cache* cache, struct cache_block* block);
static void lru_add_head(struct cache* cache, struct cache_block* block);
static struct cache_block* find_block(struct cache* cache, unsigned long long pos);
static struct cache_block* get_free_block(struct cache* cache);
static int evict_block(struct cache* cache);
// NOTE: callers should hold cache->lock for operations that modify metadata.
#endif

// DEFINITIONS
/**
 * @brief Creates/initializes a cache with the passed backing storage device (disk) and makes it
 * available through cptr.
 * @param disk Pointer to the backing storage device.
 * @param cptr Pointer to the cache to create.
 * @return 0 on success, negative error code if error
 */
int create_cache(struct storage* disk, struct cache** cptr) {
    struct cache* cache;
    int i;
    
    if (disk == NULL || cptr == NULL) {
        return -EINVAL;
    }
    
    // Allocate cache structure
    cache = kmalloc(sizeof(struct cache));
    if (cache == NULL) {
        return -ENOMEM;
    }
    
    // Allocate cache blocks array
    cache->blocks = kmalloc(CACHE_CAPACITY * sizeof(struct cache_block));
    if (cache->blocks == NULL) {
        kfree(cache);
        return -ENOMEM;
    }
    
    // Initialize cache
    cache->disk = disk;
    cache->capacity = CACHE_CAPACITY;
    cache->used = 0;
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    lock_init(&cache->lock);
    condition_init(&cache->cond, "cache_cond");
    
    // Initialize all blocks
    for (i = 0; i < CACHE_CAPACITY; i++) {
        cache->blocks[i].pos = 0;
        cache->blocks[i].data = NULL;
        cache->blocks[i].dirty = 0;
        cache->blocks[i].valid = 0;
        cache->blocks[i].next = NULL;
        cache->blocks[i].prev = NULL;
    cache->blocks[i].refcnt = 0;
    cache->blocks[i].loading = 0;
    }
    
    *cptr = cache;
    return 0;
}

#ifdef DEBUG_CACHE

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
    // FIXME
    if (cache == NULL || pptr == NULL) return -EINVAL;
    if (pos % CACHE_BLKSZ != 0) return -EINVAL;

    void* buffer = kmalloc(CACHE_BLKSZ);
    *pptr = buffer;
    long len = storage_fetch(cache->disk, pos, buffer, CACHE_BLKSZ); // calls wait
    if (len < 0) return (int)len;

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
    // FIXME
    if (pblk == NULL || cache == NULL) return;
    kfree(pblk);
    return;
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // FIXME
    return -ENOTSUP;
}

#else
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
    struct cache_block* block;
    struct cache_block* freeb;
    void* buf = NULL;
    long result;

    if (cache == NULL || pptr == NULL) return -EINVAL;
    if (pos % CACHE_BLKSZ != 0) return -EINVAL;

    lock_acquire(&cache->lock);

    while (1) {
        block = find_block(cache, pos);
        
        if (block != NULL) {
            /* Block exists in cache (either valid or being loaded) */
            if (block->loading) {
                /* Another thread is loading this block - wait for completion */
                condition_wait(&cache->cond);
                continue; /* Re-check from beginning after waking */
            }
            
            if (block->valid) {
                /* Block is valid and ready to use */
                block->refcnt++;
                lru_remove(cache, block);
                lru_add_head(cache, block);
                *pptr = block->data;
                lock_release(&cache->lock);
                return 0;
            }
            /* If we get here, block exists but is neither loading nor valid - shouldn't happen */
            /* Fall through to allocate new block */
        }

        /* Block not in cache - need to allocate and load it */
        freeb = get_free_block(cache);
        if (freeb == NULL) {
            /* No free blocks - try to evict */
            int er = evict_block(cache);
            if (er != 0) {
                lock_release(&cache->lock);
                return er;
            }
            freeb = get_free_block(cache);
            if (freeb == NULL) {
                lock_release(&cache->lock);
                return -ENOMEM;
            }
        }

        /* Mark block as being loaded */
        freeb->loading = 1;
        freeb->pos = pos;
        freeb->valid = 0;
        freeb->dirty = 0;
        freeb->refcnt = 1; /* Caller will hold one reference */
        
        /* Release lock while performing I/O */
        lock_release(&cache->lock);

        /* Allocate buffer for data */
        buf = kmalloc(CACHE_BLKSZ);
        if (buf == NULL) {
            lock_acquire(&cache->lock);
            freeb->loading = 0;
            freeb->refcnt = 0;
            condition_broadcast(&cache->cond);
            lock_release(&cache->lock);
            return -ENOMEM;
        }

        /* Fetch data from backing storage */
        result = storage_fetch(cache->disk, pos, buf, CACHE_BLKSZ);
        if (result != CACHE_BLKSZ) {
            kfree(buf);
            lock_acquire(&cache->lock);
            freeb->loading = 0;
            freeb->refcnt = 0;
            condition_broadcast(&cache->cond);
            lock_release(&cache->lock);
            return -EIO;
        }

        /* Re-acquire lock and finalize block */
        lock_acquire(&cache->lock);
        
        /* Check if block is still ours (paranoid check) */
        if (freeb->loading && freeb->pos == pos) {
            freeb->data = buf;
            freeb->valid = 1;
            freeb->loading = 0;
            cache->used++;
            lru_add_head(cache, freeb);
            condition_broadcast(&cache->cond);
            *pptr = freeb->data;
            lock_release(&cache->lock);
            return 0;
        } else {
            /* Something went wrong - clean up */
            kfree(buf);
            lock_release(&cache->lock);
            return -EIO;
        }
    }
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
    struct cache_block* block = NULL;
    int i;

    if (cache == NULL || pblk == NULL) return;

    lock_acquire(&cache->lock);

    /* Find the block containing this data pointer */
    for (i = 0; i < CACHE_CAPACITY; i++) {
        if (cache->blocks[i].valid && 
            cache->blocks[i].data == pblk) {
            block = &cache->blocks[i];
            break;
        }
    }

    if (block == NULL) {
        /* Block not found - this shouldn't happen but handle gracefully */
        lock_release(&cache->lock);
        return;
    }

    /* Mark dirty if requested (only set, never clear) */
    if (dirty) {
        block->dirty = 1;
    }

    /* Decrement reference count */
    if (block->refcnt > 0) {
        block->refcnt--;
        
        /* Only move in LRU if refcnt reaches 0 (no longer in active use) */
        if (block->refcnt == 0) {
            lru_remove(cache, block);
            lru_add_head(cache, block);
        }
    }

    /* Wake any threads waiting for blocks to become available */
    condition_broadcast(&cache->cond);

    lock_release(&cache->lock);
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    struct cache_block* block;
    long result;
    int i;
    int error = 0;

    if (cache == NULL) return -EINVAL;

    /* Flush all dirty blocks */
    for (i = 0; i < CACHE_CAPACITY; i++) {
        lock_acquire(&cache->lock);
        block = &cache->blocks[i];

        /* Skip if not dirty or not valid */
        if (!block->valid || !block->dirty || block->loading) {
            lock_release(&cache->lock);
            continue;
        }

        /* Wait for block to have no users */
        while (block->refcnt > 0) {
            condition_wait(&cache->cond);
        }

        /* Mark as being written back */
        block->loading = 1;
        void* data_copy = block->data;
        unsigned long long pos_copy = block->pos;
        lock_release(&cache->lock);

        /* Perform write without holding lock */
        result = storage_store(cache->disk, pos_copy, data_copy, CACHE_BLKSZ);
        
        lock_acquire(&cache->lock);
        if (result == CACHE_BLKSZ) {
            /* Success - clear dirty flag */
            block->dirty = 0;
        } else {
            /* Failed - record error but continue flushing other blocks */
            error = -EIO;
        }
        block->loading = 0;
        condition_broadcast(&cache->cond);
        lock_release(&cache->lock);
    }

    return error;
}

// INTERNAL(HELPER) FUNCTION DEFINITIONS
//

/**
 * @brief Remove a block from the LRU list
 * @param cache Pointer to the cache
 * @param block Pointer to the block to remove
 */
static void lru_remove(struct cache* cache, struct cache_block* block) {
    if (block == NULL) return;
    
    // Update head if removing head
    if (cache->lru_head == block) {
        cache->lru_head = block->next;
    }
    
    // Update tail if removing tail
    if (cache->lru_tail == block) {
        cache->lru_tail = block->prev;
    }
    
    // Update neighbors
    if (block->prev != NULL) {
        block->prev->next = block->next;
    }
    if (block->next != NULL) {
        block->next->prev = block->prev;
    }
    
    // Clear block's links
    block->next = NULL;
    block->prev = NULL;
}

/**
 * @brief Add a block to the head of the LRU list
 * @param cache Pointer to the cache
 * @param block Pointer to the block to add
 */
static void lru_add_head(struct cache* cache, struct cache_block* block) {
    if (block == NULL) return;
    
    // Clear block's links first
    block->next = NULL;
    block->prev = NULL;
    
    // If list is empty
    if (cache->lru_head == NULL) {
        cache->lru_head = block;
        cache->lru_tail = block;
        return;
    }
    
    // Add to head
    block->next = cache->lru_head;
    cache->lru_head->prev = block;
    cache->lru_head = block;
}

/**
 * @brief Find a block in the cache by position
 * @param cache Pointer to the cache
 * @param pos Position to search for
 * @return Pointer to the block if found, NULL otherwise
 */
static struct cache_block* find_block(struct cache* cache, unsigned long long pos) {
    int i;
    
    for (i = 0; i < CACHE_CAPACITY; i++) {
        /* Match blocks that are either valid or currently being loaded for this pos */
        if ((cache->blocks[i].valid || cache->blocks[i].loading) && cache->blocks[i].pos == pos) {
            return &cache->blocks[i];
        }
    }
    
    return NULL;
}

/**
 * @brief Get a free block from the cache
 * @param cache Pointer to the cache
 * @return Pointer to a free block, NULL if none available
 */
static struct cache_block* get_free_block(struct cache* cache) {
    int i;
    
    for (i = 0; i < CACHE_CAPACITY; i++) {
        /* free if not valid and not currently being used/loaded */
        if (!cache->blocks[i].valid && !cache->blocks[i].loading && cache->blocks[i].refcnt == 0) {
            return &cache->blocks[i];
        }
    }
    
    return NULL;
}

/**
 * @brief Evict the least recently used block
 * @param cache Pointer to the cache
 * @return 0 on success, negative error code on error
 */
static int evict_block(struct cache* cache) {
    struct cache_block* victim = NULL;
    struct cache_block* current;
    void* data_to_write = NULL;
    unsigned long long pos_to_write = 0;
    int need_writeback = 0;
    long result;

    /* Scan from tail (LRU) to find an evictable block */
    current = cache->lru_tail;
    while (current != NULL) {
        if (current->valid && 
            current->refcnt == 0 && 
            !current->loading) {
            victim = current;
            break;
        }
        current = current->prev;
    }

    if (victim == NULL) {
        return -EBUSY; /* No evictable blocks */
    }

    /* Remove from LRU and prepare for eviction */
    lru_remove(cache, victim);
    victim->loading = 1; /* Prevent others from using it */

    /* Check if writeback needed */
    if (victim->dirty) {
        need_writeback = 1;
        data_to_write = victim->data;
        pos_to_write = victim->pos;
    }

    /* Clear the block (still holding lock) */
    void* old_data = victim->data;
    victim->data = NULL;
    victim->valid = 0;
    victim->dirty = 0;
    victim->pos = 0;
    cache->used--;

    /* Release lock for I/O */
    lock_release(&cache->lock);

    /* Perform writeback if needed */
    if (need_writeback) {
        result = storage_store(cache->disk, pos_to_write, data_to_write, CACHE_BLKSZ);
        if (result != CACHE_BLKSZ) {
            /* Writeback failed - free memory and return error */
            kfree(old_data);
            lock_acquire(&cache->lock);
            victim->loading = 0;
            condition_broadcast(&cache->cond);
            /* Note: lock remains held for caller */
            return -EIO;
        }
    }

    /* Free the old buffer */
    if (old_data != NULL) {
        kfree(old_data);
    }

    /* Re-acquire lock and finish */
    lock_acquire(&cache->lock);
    victim->loading = 0;
    condition_broadcast(&cache->cond);
    /* Lock remains held for caller */
    return 0;
}
#endif