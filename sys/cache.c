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
};

// INTERNAL FUNCTION DECLARATIONS
//

static void lru_remove(struct cache* cache, struct cache_block* block);
static void lru_add_head(struct cache* cache, struct cache_block* block);
static struct cache_block* find_block(struct cache* cache, unsigned long long pos);
static struct cache_block* get_free_block(struct cache* cache);
static int evict_block(struct cache* cache);

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
    
    // Initialize all blocks
    for (i = 0; i < CACHE_CAPACITY; i++) {
        cache->blocks[i].pos = 0;
        cache->blocks[i].data = NULL;
        cache->blocks[i].dirty = 0;
        cache->blocks[i].valid = 0;
        cache->blocks[i].next = NULL;
        cache->blocks[i].prev = NULL;
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
    long result;
    
    if (cache == NULL || pptr == NULL) {
        return -EINVAL;
    }
    
    // Check if position is aligned to block size
    if (pos % CACHE_BLKSZ != 0) {
        return -EINVAL;
    }
    
    // Look for existing block
    block = find_block(cache, pos);
    if (block != NULL) {
        // Block found in cache, move to head of LRU list
        lru_remove(cache, block);
        lru_add_head(cache, block);
        *pptr = block->data;
        return 0;
    }
    
    // Block not in cache, need to load it
    block = get_free_block(cache);
    if (block == NULL) {
        // No free blocks, need to evict
        if (evict_block(cache) != 0) {
            return -EIO;
        }
        block = get_free_block(cache);
        if (block == NULL) {
            return -ENOMEM;
        }
    }
    
    // Allocate data buffer for the block
    block->data = kmalloc(CACHE_BLKSZ);
    if (block->data == NULL) {
        return -ENOMEM;
    }
    
    // Read block from storage
    result = storage_fetch(cache->disk, pos, block->data, CACHE_BLKSZ);
    if (result != CACHE_BLKSZ) {
        kfree(block->data);
        block->data = NULL;
        return -EIO;
    }
    
    // Update block metadata
    block->pos = pos;
    block->dirty = 0;
    block->valid = 1;
    cache->used++;
    
    // Add to head of LRU list
    lru_add_head(cache, block);
    
    *pptr = block->data;
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
    struct cache_block* block;
    int i;
    
    if (cache == NULL || pblk == NULL) {
        return;
    }
    
    // Find the block containing this data
    for (i = 0; i < CACHE_CAPACITY; i++) {
        if (cache->blocks[i].data == pblk && cache->blocks[i].valid) {
            block = &cache->blocks[i];
            
            // Update dirty flag
            if (dirty) {
                block->dirty = 1;
            }
            
            // Move to head of LRU list (most recently used)
            lru_remove(cache, block);
            lru_add_head(cache, block);
            
            return;
        }
    }
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
    
    if (cache == NULL) {
        return -EINVAL;
    }
    
    // Flush all dirty blocks
    for (i = 0; i < CACHE_CAPACITY; i++) {
        block = &cache->blocks[i];
        if (block->valid && block->dirty) {
            result = storage_store(cache->disk, block->pos, block->data, CACHE_BLKSZ);
            if (result != CACHE_BLKSZ) {
                return -EIO;
            }
            block->dirty = 0;
        }
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
        if (cache->blocks[i].valid && cache->blocks[i].pos == pos) {
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
        if (!cache->blocks[i].valid) {
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
    struct cache_block* block;
    long result;
    
    if (cache->lru_tail == NULL) {
        return -ENOMEM;
    }
    
    block = cache->lru_tail;
    
    // If block is dirty, write it back to storage
    if (block->dirty) {
        result = storage_store(cache->disk, block->pos, block->data, CACHE_BLKSZ);
        if (result != CACHE_BLKSZ) {
            return -EIO;
        }
    }
    
    // Remove from LRU list
    lru_remove(cache, block);
    
    // Free data and reset block
    if (block->data != NULL) {
        kfree(block->data);
    }
    block->data = NULL;
    block->pos = 0;
    block->dirty = 0;
    block->valid = 0;
    cache->used--;
    
    return 0;
}
