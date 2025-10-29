/*! @file ktfs.c
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois
*/

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "ktfs.h"

#include "cache.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "filesys.h"
#include "fsimpl.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"
#include "uioimpl.h"

// INTERNAL TYPE DEFINITIONS
//

/// @brief File struct for a file in the Keegan Teal Filesystem
struct ktfs_file {
    struct uio                  uio; /* must be first so uio_init0 cast works */
    uint64_t                    file_size;
    struct ktfs_dir_entry       dentry;
    uint64_t                    pos;
    struct ktfs_filesystem*     fs;
    struct ktfs_file*           next;
    struct ktfs_file*           prev;
    int                         dirty;
};

struct ktfs_file_list {
    struct ktfs_file* head;
    struct ktfs_file* tail;
};

static struct ktfs_file_list open_file_list = {
    .head = NULL,
    .tail = NULL
};

/// KTFS File System struct
struct ktfs_filesystem {
    struct filesystem intf; 
    struct cache* backing_cache;
    struct ktfs_superblock sb;
    struct ktfs_inode root_inode;
    struct ktfs_filesystem* next;
    struct ktfs_file_list*  open_files;
};

struct ktfs_filesystem_list {
    struct ktfs_filesystem* head;
    struct ktfs_filesystem* tail;
};

static struct ktfs_filesystem_list fs_list = {
    .head = NULL,
    .tail = NULL
};

// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr);
void ktfs_close(struct uio* uio);
int ktfs_cntl(struct uio* uio, int cmd, void* arg);
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len);
long ktfs_store(struct uio* uio, const void* buf, unsigned long len);
int ktfs_create(struct filesystem* fs, const char* name);
int ktfs_delete(struct filesystem* fs, const char* name);
void ktfs_flush(struct filesystem* fs);

void ktfs_listing_close(struct uio* uio);
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz);

static const struct uio_intf ktfs_uio_intf = {
    .close = ktfs_close,
    .read  = ktfs_fetch,
    .write = ktfs_store,
    .cntl  = ktfs_cntl
};
/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */
int mount_ktfs(const char* name, struct cache* cache) {
    struct ktfs_filesystem* fs = (struct ktfs_filesystem*)kmalloc(sizeof(struct ktfs_filesystem));
    fs->backing_cache = cache;

    // need to read superblock?
    uint8_t* sb_ptr;
    cache_get_block(cache, 0, (void**)&sb_ptr);
    struct ktfs_superblock* disk_sb = (struct ktfs_superblock*) sb_ptr;
    fs->sb = *disk_sb;
    //CHANGED: Release superblock after copying data - we no longer need the cache block
    cache_release_block(cache, sb_ptr, 0);

    uint32_t inode_table_start_block = 1 + fs->sb.inode_bitmap_block_count + fs->sb.bitmap_block_count;

    uint8_t* inode_block_ptr;
    cache_get_block(cache, inode_table_start_block, (void**)&inode_block_ptr);
    fs->root_inode = *((struct ktfs_inode*) inode_block_ptr);
    //CHANGED: Release inode block after copying root inode data
    cache_release_block(cache, inode_block_ptr, 0);

    fs->intf.open = ktfs_open;
    fs->intf.create = ktfs_create;
    fs->intf.delete = ktfs_delete;
    fs->intf.flush = ktfs_flush;

    if (!fs_list.head) {
        fs_list.head = fs;
        fs_list.tail = fs;
        fs->next = NULL;
    } else {
        fs_list.tail->next = fs;
        fs_list.tail = fs;
        fs->next = NULL;
    }

    /* pass the embedded filesystem interface to match attach_filesystem prototype */
    attach_filesystem(name, &fs->intf);
    //CHANGED: Return 0 on success instead of -ENOTSUP
    return 0;
}

/**
 * @brief Opens a file or ls (listing) with the given name and returns a pointer to the uio through
 * the double pointer
 * @param name The name of the file to open or "\" for listing (CP3)
 * @param uioptr Will return a pointer to a file or ls (list) uio pointer through this double
 * pointer
 * @return 0 if open successful, negative error code if error
 */
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) {
    struct ktfs_filesystem* ktfs_fs = (struct ktfs_filesystem*) fs;
    
    struct ktfs_superblock sb = ktfs_fs->sb;
    uint32_t inode_table_start_block = 1 + sb.inode_bitmap_block_count + sb.bitmap_block_count;
    uint32_t inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ;
    uint32_t dentries_per_block = KTFS_BLKSZ / KTFS_DENSZ;
    
    uint32_t total_dentries = ktfs_fs->root_inode.size / KTFS_DENSZ;

    for (int i = 0; i < total_dentries; i++) {
        uint32_t dentry_block_index = i / dentries_per_block;
        uint32_t dentry_index_in_block = i % dentries_per_block;
        uint8_t* data_block;

        if (dentry_block_index < KTFS_NUM_DIRECT_DATA_BLOCKS) {
            cache_get_block(ktfs_fs->backing_cache, ktfs_fs->root_inode.block[dentry_block_index], (void**)&data_block);
        } else if (dentry_block_index < KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ / sizeof(uint32_t))) {
            uint32_t indirect_index = dentry_block_index - KTFS_NUM_DIRECT_DATA_BLOCKS;
            uint8_t* indirect_block;
            cache_get_block(ktfs_fs->backing_cache, ktfs_fs->root_inode.indirect, (void**)&indirect_block);
            uint32_t* indirect_block_ptrs = (uint32_t*) indirect_block;
            uint32_t data_block_index = indirect_block_ptrs[indirect_index];
            //CHANGED: Release indirect block after getting data block index
            cache_release_block(ktfs_fs->backing_cache, indirect_block, 0);
            cache_get_block(ktfs_fs->backing_cache, data_block_index, (void**)&data_block);
        } else {
            uint32_t PTRS_PER_BLOCK = KTFS_BLKSZ / sizeof(uint32_t);
            uint32_t base = KTFS_NUM_DIRECT_DATA_BLOCKS + PTRS_PER_BLOCK;
            uint32_t raw_index = dentry_block_index - base;
            uint32_t dindirect_index = raw_index / (PTRS_PER_BLOCK * PTRS_PER_BLOCK);
            uint32_t indirect_index = (raw_index / PTRS_PER_BLOCK) % PTRS_PER_BLOCK;
            uint32_t direct_index = raw_index % PTRS_PER_BLOCK;
            
            uint8_t* dindirect_block;
            cache_get_block(ktfs_fs->backing_cache, ktfs_fs->root_inode.dindirect[dindirect_index], (void**)&dindirect_block);
            uint32_t* indirect_ptrs = (uint32_t*) dindirect_block;
            
            uint8_t* indirect_block;
            cache_get_block(ktfs_fs->backing_cache, indirect_ptrs[indirect_index], (void**)&indirect_block);
            //CHANGED: Release dindirect block after getting indirect block index
            cache_release_block(ktfs_fs->backing_cache, dindirect_block, 0);
            
            uint32_t* data_ptrs = (uint32_t*) indirect_block;
            uint32_t data_block_index = data_ptrs[direct_index];
            //CHANGED: Release indirect block after getting data block index
            cache_release_block(ktfs_fs->backing_cache, indirect_block, 0);
            
            cache_get_block(ktfs_fs->backing_cache, data_block_index, (void**)&data_block);
        }
        
        struct ktfs_dir_entry* dentry_ptr = (struct ktfs_dir_entry*)(data_block + (dentry_index_in_block * KTFS_DENSZ));
        
        if (strcmp(dentry_ptr->name, name) == 0) {
            uint16_t inode_number = dentry_ptr->inode;
            uint32_t inode_block_index = inode_number / inodes_per_block;
            uint32_t inode_index_in_block = inode_number % inodes_per_block;
            uint8_t* inode_block;
            cache_get_block(ktfs_fs->backing_cache, inode_table_start_block + inode_block_index, (void**)&inode_block);
            struct ktfs_inode* inode_ptr = (struct ktfs_inode*)(inode_block + (inode_index_in_block * KTFS_INOSZ));
            
            struct ktfs_file* open_file = kmalloc(sizeof(struct ktfs_file));
            uio_init0((struct uio*) open_file, &ktfs_uio_intf);
            open_file->file_size = inode_ptr->size;
            open_file->fs = ktfs_fs;
            open_file->pos = 0;
            open_file->dirty = 0;
            open_file->dentry = *dentry_ptr;
            
            //CHANGED: Release inode block after copying inode data
            cache_release_block(ktfs_fs->backing_cache, inode_block, 0);
            //CHANGED: Release data block (dentry block) after copying dentry
            cache_release_block(ktfs_fs->backing_cache, data_block, 0);
            
            if (open_file_list.head == NULL) {
                open_file_list.head = open_file;
                open_file_list.tail = open_file;
                open_file->prev = NULL;
            } else {
                open_file_list.tail->next = open_file;
                open_file->prev = open_file_list.tail;
                open_file_list.tail = open_file;
            }
            open_file->next = NULL;
            
            //CHANGED: Set uioptr to point to the opened file
            *uioptr = (struct uio*)open_file;
            //CHANGED: Return 0 on success
            return 0;
        }
        
        //CHANGED: Release data block at end of each iteration if file not found yet
        cache_release_block(ktfs_fs->backing_cache, data_block, 0);
    }

    //CHANGED: Return -ENOENT (file not found) instead of -ENOTSUP
    return -ENOENT;
}

/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */
void ktfs_close(struct uio* uio) {
    if (uio == NULL) {
        return;
    }

    struct ktfs_file* file = (struct ktfs_file*) uio;
    
    if (file->prev == NULL && file->next == NULL) {
        open_file_list.head = NULL;
        open_file_list.tail = NULL;
    } else if (file->prev == NULL) {
        open_file_list.head = file->next;
        file->next->prev = NULL;
    } else if (file->next == NULL) {
        file->prev->next = NULL;
        open_file_list.tail = file->prev;
    } else {
        file->prev->next = file->next;
        file->next->prev = file->prev;
    }

    //CHANGED: Call cache_flush instead of ktfs_flush (which is empty)
    cache_flush(file->fs->backing_cache);

    kfree(file);
}

/*
Helper function for locating data block and putting it in
*/
void ktfs_get_block(struct ktfs_filesystem* fs, uint16_t inode_number, uint32_t block_num, uint8_t** data_block_ptr) {
    uint32_t inode_table_start_block = 1 + fs->sb.inode_bitmap_block_count + fs->sb.bitmap_block_count;

    uint32_t inode_block_index = inode_number / (KTFS_BLKSZ / KTFS_INOSZ);
    uint32_t inode_index_in_block = inode_number % (KTFS_BLKSZ / KTFS_INOSZ);
    uint8_t* inode_block;

    cache_get_block(fs->backing_cache, inode_table_start_block + inode_block_index, (void**)&inode_block);
    struct ktfs_inode* inode_ptr = (struct ktfs_inode*)(inode_block + (inode_index_in_block * KTFS_INOSZ));
    
    if (block_num < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        //CHANGED: Release inode block before getting data block (we're done with it)
        uint32_t data_block_index = inode_ptr->block[block_num];
        cache_release_block(fs->backing_cache, inode_block, 0);
        cache_get_block(fs->backing_cache, data_block_index, (void**) data_block_ptr);
    } else if (block_num < KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ / sizeof(uint32_t))) {
        uint32_t indirect_index = block_num - KTFS_NUM_DIRECT_DATA_BLOCKS;
        uint8_t* indirect_block;
        //CHANGED: Get indirect block index before releasing inode block
        uint32_t indirect_block_index = inode_ptr->indirect;
        cache_release_block(fs->backing_cache, inode_block, 0);
        
    cache_get_block(fs->backing_cache, indirect_block_index, (void**)&indirect_block);
        uint32_t* indirect_block_ptrs = (uint32_t*) indirect_block;
        uint32_t data_block_index = indirect_block_ptrs[indirect_index];
        //CHANGED: Release indirect block after getting data block index
        cache_release_block(fs->backing_cache, indirect_block, 0);
        cache_get_block(fs->backing_cache, data_block_index, (void**) data_block_ptr);
    } else {
        uint32_t PTRS_PER_BLOCK = KTFS_BLKSZ / sizeof(uint32_t);
        uint32_t base = KTFS_NUM_DIRECT_DATA_BLOCKS + PTRS_PER_BLOCK;
        uint32_t raw_index = block_num - base;
        uint32_t dindirect_index = raw_index / (PTRS_PER_BLOCK * PTRS_PER_BLOCK);
        uint32_t indirect_index = (raw_index / PTRS_PER_BLOCK) % PTRS_PER_BLOCK;
        uint32_t direct_index = raw_index % PTRS_PER_BLOCK;
        
        //CHANGED: Get dindirect block index before releasing inode block
        uint32_t dindirect_block_index = inode_ptr->dindirect[dindirect_index];
        cache_release_block(fs->backing_cache, inode_block, 0);
        
        uint8_t* dindirect_block;
    cache_get_block(fs->backing_cache, dindirect_block_index, (void**)&dindirect_block);
        uint32_t* indirect_ptrs = (uint32_t*) dindirect_block;
        uint32_t indirect_block_index = indirect_ptrs[indirect_index];
        //CHANGED: Release dindirect block after getting indirect block index
        cache_release_block(fs->backing_cache, dindirect_block, 0);
        
    uint8_t* indirect_block;
    cache_get_block(fs->backing_cache, indirect_block_index, (void**)&indirect_block);
        uint32_t* data_ptrs = (uint32_t*) indirect_block;
        uint32_t data_block_index = data_ptrs[direct_index];
        //CHANGED: Release indirect block after getting data block index
        cache_release_block(fs->backing_cache, indirect_block, 0);
        
        cache_get_block(fs->backing_cache, data_block_index, (void**) data_block_ptr);
    }
    //CHANGED: NOTE - We do NOT release data_block_ptr here because caller needs to use it
}

/**
 * @brief Reads data from file attached to uio into provided argument buffer
 * @param uio uio of file to be read
 * @param buf Buffer to be filled
 * @param len Number of bytes to read
 * @return Number of bytes read if successful, negative error code if error
 */
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len) {
    struct ktfs_file* file = (struct ktfs_file*) uio;
    
    //CHANGED: can't use min without library, return 0 at EOF (not an error);
    if (len > file->file_size - file->pos) {
    len = file->file_size - file->pos;
    }
    if (len == 0) {
        return 0;  // At EOF, return 0 bytes read
    }
    
    //CHANGED: Rewritten to handle partial blocks correctly
    uint32_t bytes_copied = 0;
    uint32_t current_pos = file->pos;
    
    while (bytes_copied < len) {
        // Which block are we reading from?
        uint32_t block_num = current_pos / KTFS_BLKSZ;
        // Offset within that block
        uint32_t offset_in_block = current_pos % KTFS_BLKSZ;
        // How many bytes to copy from this block LOWKEY NOT SURE IF WE CAN USE std::min .-. 
        uint32_t bytes_to_end_of_block = KTFS_BLKSZ - offset_in_block;
    uint32_t remaining_len = len - bytes_copied;
    uint32_t bytes_in_block;

    if (bytes_to_end_of_block < remaining_len) {
        bytes_in_block = bytes_to_end_of_block;
    } else {
        bytes_in_block = remaining_len;
    }

        
        // Get the block
    uint8_t* data_block;
    ktfs_get_block(file->fs, file->dentry.inode, block_num, &data_block);
        
        // Copy the relevant portion
        memcpy(buf + bytes_copied, data_block + offset_in_block, bytes_in_block);
        
        //CHANGED: Release the block after copying (dirty=0 because we only read)
        cache_release_block(file->fs->backing_cache, data_block, 0);
        
        // Update counters
        bytes_copied += bytes_in_block;
        current_pos += bytes_in_block;
    }
    
    file->pos = current_pos;
    return bytes_copied;
}

/**
 * @brief Write data from the provided argument buffer into file attached to uio
 * @param uio The file to be written to
 * @param buf The buffer to be read from
 * @param len Number of bytes to write from the buffer to the file
 * @return Number of bytes written from the buffer to the file system if sucessful, negative error
 * code if error
 */
long ktfs_store(struct uio* uio, const void* buf, unsigned long len) {
    // FIXME
    return -ENOTSUP;
}

/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */
int ktfs_create(struct filesystem* fs, const char* name) {
    // FIXME
    return -ENOTSUP;
}

/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */
int ktfs_delete(struct filesystem* fs, const char* name) {
    // FIXME
    return -ENOTSUP;
}

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions
 * @details Any commands such as (FCNTL_GETEND, FCNTL_GETPOS, ...) should pass back through the arg
 * variable. Do not directly return the value.
 * @details FCNTL_GETEND should pass back the size of the file in bytes through the arg variable.
 * @details FCNTL_SETEND should set the size of the file to the value passed in through arg.
 * @details FCNTL_GETPOS should pass back the current position of the file pointer in bytes through
 * the arg variable.
 * @details FCNTL_SETPOS should set the current position of the file pointer to the value passed in
 * through arg.
 * @param uio the uio object of the file to perform the control function
 * @param cmd the operation to execute. KTFS should support FCNTL_GETEND, FCNTL_SETEND (CP2),
 * FCNTL_GETPOS, FCNTL_SETPOS.
 * @param arg the argument to pass in, may be different for different control functions
 * @return 0 if successful, negative error code if error
 */
int ktfs_cntl(struct uio* uio, int cmd, void* arg) {
    //CHANGED: Implemented FCNTL commands for CP1
    struct ktfs_file* file = (struct ktfs_file*) uio;
    
    if (file == NULL) {
        return -EINVAL;
    }
    
    switch (cmd) {
        case FCNTL_GETEND:
            //CHANGED: Pass back file size through arg
            *((uint64_t*)arg) = file->file_size;
            return 0;
            
        case FCNTL_GETPOS:
            //CHANGED: Pass back current position through arg
            *((uint64_t*)arg) = file->pos;
            return 0;
            
        case FCNTL_SETPOS:
            //CHANGED: Set current position from arg
            file->pos = *((uint64_t*)arg);
            return 0;
            
        case FCNTL_SETEND:
            //CHANGED: Not implemented for CP1 (read-only filesystem)
            return -ENOTSUP;
            
        default:
            return -EINVAL;
    }
}

/**
 * @brief Flushes the cache to the backing device
 * @return 0 if flush successful, negative error code if error
 */
void ktfs_flush(struct filesystem* fs) {
    //CHANGED: Implemented to actually flush the cache
    struct ktfs_filesystem* ktfs_fs = (struct ktfs_filesystem*) fs;
    cache_flush(ktfs_fs->backing_cache);
}

/**
 * @brief Closes the listing device represented by the uio pointer
 * @param uio The uio pointer of ls
 * @return None
 */
void ktfs_listing_close(struct uio* uio) {
    // FIXME
    return;
}

/**
 * @brief Reads all of the files names in the file system using ls and copies them into the
 * providied buffer
 * @param uio The uio pointer of ls
 * @param buf The buffer to copy the file names to
 * @param bufsz The size of the buffer
 * @return The size written to the buffer
 */
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // FIXME
    return -ENOTSUP;
}