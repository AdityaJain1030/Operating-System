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

static void lru_remove(struct cache* cache, struct cache_block* block);
static void lru_add_head(struct cache* cache, struct cache_block* block);
static struct cache_block* find_block(struct cache* cache, unsigned long long pos);
static struct cache_block* get_free_block(struct cache* cache);
static int evict_block(struct cache* cache);
// NOTE: callers should hold cache->lock for operations that modify metadata.

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

    /* Look for existing block (including blocks being loaded) */
    while (1) {
        block = find_block(cache, pos);
        if (block != NULL) {
            if (block->valid) {
                /* in-cache: bump refcnt and move to head */
                block->refcnt++;
                lru_remove(cache, block);
                lru_add_head(cache, block);
                *pptr = block->data;
                lock_release(&cache->lock);
                return 0;
            }

            /* block is being loaded by another thread; wait for it without
               holding the cache lock (yielding to the loader). This avoids
               deadlock because condition_wait does not atomically release
               the cache lock. */
            while (block->loading) {
                lock_release(&cache->lock);
                running_thread_yield();
                lock_acquire(&cache->lock);
                /* re-find block in case cache changed while we yielded */
                block = find_block(cache, pos);
                if (block == NULL) break;
            }

            /* after wakeup/yield, loop to re-check */
            continue;
        }

        /* Not present. Try to find a free block */
        freeb = get_free_block(cache);
        if (freeb == NULL) {
            /* Need to evict; evict_block expects caller to hold lock */
            int er = evict_block(cache);
            if (er != 0) {
                lock_release(&cache->lock);
                return er;
            }
            /* Try again for a free block */
            freeb = get_free_block(cache);
            if (freeb == NULL) {
                lock_release(&cache->lock);
                return -ENOMEM;
            }
        }

        /* Reserve the block for loading */
        freeb->loading = 1;
        freeb->pos = pos;
        freeb->valid = 0;
        freeb->refcnt = 1; /* caller holds one ref */
        lock_release(&cache->lock);

        /* Allocate buffer and fetch while not holding lock */
        buf = kmalloc(CACHE_BLKSZ);
        if (buf == NULL) {
            lock_acquire(&cache->lock);
            freeb->loading = 0;
            freeb->refcnt = 0;
            condition_broadcast(&cache->cond);
            lock_release(&cache->lock);
            return -ENOMEM;
        }

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

        /* Install block into cache */
        lock_acquire(&cache->lock);
        freeb->data = buf;
        freeb->dirty = 0;
        freeb->valid = 1;
        freeb->loading = 0;
        cache->used++;
        lru_add_head(cache, freeb);
        condition_broadcast(&cache->cond);
        *pptr = freeb->data;
        lock_release(&cache->lock);
        return 0;
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

    /* Find the block containing this data */
    for (i = 0; i < CACHE_CAPACITY; i++) {
        if (cache->blocks[i].data == pblk && cache->blocks[i].valid) {
            block = &cache->blocks[i];
            break;
        }
    }

    if (block == NULL) {
        lock_release(&cache->lock);
        return;
    }

    /* Update dirty flag */
    if (dirty) block->dirty = 1;

    /* Decrement user reference */
    if (block->refcnt > 0) block->refcnt--;

    /* Move to head of LRU list (most recently used) */
    lru_remove(cache, block);
    lru_add_head(cache, block);

    /* Wake any waiters (evict or other getters) */
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

    if (cache == NULL) return -EINVAL;

    for (i = 0; i < CACHE_CAPACITY; i++) {
        lock_acquire(&cache->lock);
        block = &cache->blocks[i];

        if (!(block->valid && block->dirty && !block->loading)) {
            lock_release(&cache->lock);
            continue;
        }

        /* mark block busy for writeback */
        block->loading = 1;
        void* data_ptr = block->data;
        unsigned long long pos = block->pos;
        lock_release(&cache->lock);

        result = storage_store(cache->disk, pos, data_ptr, CACHE_BLKSZ);
        if (result != CACHE_BLKSZ) {
            /* restore loading flag and return error */
            lock_acquire(&cache->lock);
            block->loading = 0;
            lock_release(&cache->lock);
            return -EIO;
        }

        lock_acquire(&cache->lock);
        block->dirty = 0;
        block->loading = 0;
        condition_broadcast(&cache->cond);
        lock_release(&cache->lock);
    }

    return 0;
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
    struct cache_block* block = cache->lru_tail;
    struct cache_block* cand = NULL;
    void* data_ptr = NULL;
    unsigned long long pos = 0;
    int was_dirty = 0;
    long result;

    /* Find a candidate from the tail that is not in use and not loading */
    while (block != NULL) {
        if (block->refcnt == 0 && !block->loading) {
            cand = block;
            break;
        }
        block = block->prev;
    }

    if (cand == NULL) {
        return -EBUSY; /* nothing we can evict now */
    }

    /* Remove from LRU and mark as being evicted */
    lru_remove(cache, cand);
    cand->loading = 1; /* mark busy */
    cand->valid = 0;
    cache->used--;

    /* Steal metadata */
    data_ptr = cand->data;
    pos = cand->pos;
    was_dirty = cand->dirty;

    /* Clear metadata while we perform I/O without holding the lock */
    cand->data = NULL;
    cand->pos = 0;
    cand->dirty = 0;
    cand->refcnt = 0;
    /* Release lock while performing I/O */
    lock_release(&cache->lock);

    /* Do write-back if necessary (without holding cache lock) */
    if (was_dirty && data_ptr != NULL) {
        result = storage_store(cache->disk, pos, data_ptr, CACHE_BLKSZ);
        if (result != CACHE_BLKSZ) {
            /* Acquire lock to restore loading flag and return error (leave lock held)
               so caller can decide how to continue. */
            lock_acquire(&cache->lock);
            cand->loading = 0;
            condition_broadcast(&cache->cond);
            return -EIO;
        }
    }

    if (data_ptr != NULL) kfree(data_ptr);

    lock_acquire(&cache->lock);
    cand->loading = 0;
    condition_broadcast(&cache->cond);
    /* Leave lock held for caller */
    return 0;
}
