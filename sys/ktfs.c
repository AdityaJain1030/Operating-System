// ktfs.c
// KTFS filesystem implementation.
// Copyright (c) 2024-2025 University of Illinois
//got rid of sync as they are already implemented in cache
#define BLOCK_NUM_ERROR 0xFFFFFFFF
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

// Represents any open file in KTFS
struct ktfs_file {
    struct uio uio;                    // UIO interface (must be first)
    struct filesystem* fs;             // Parent filesystem
    struct ktfs_dir_entry dentry;      // Directory entry (name & inode number)
    struct ktfs_inode inode;           // Cached inode data
    uint32_t pos;                      // Current file position
    uint32_t size;                     // File size in bytes
    struct ktfs_file* next;            // Next file in open files list
};

// KTFS filesys
struct ktfs {
    struct filesystem fs;              // Filesystem interface (must be first)
    struct cache* cache;               // Cache for block access
    struct ktfs_superblock superblock; // Cached superblock
    struct ktfs_file* open_files_head; // fixed: Head of open files ll
    int num_open_files;                // Number of currently open files
    struct lock fs_lock;               // Filesystem-wide lock
};

// INTERNAL FUNCTION DECLARATIONS
//

static int read_superblock(struct ktfs* ktfs);
static int read_inode(struct ktfs* ktfs, uint16_t inode_num, struct ktfs_inode* inode);
static int find_file_in_directory(struct ktfs* ktfs, const char* name, struct ktfs_dir_entry* dentry);
static uint32_t get_data_block_number(struct ktfs* ktfs, struct ktfs_inode* inode, uint32_t file_block_idx);
static int read_file_data(struct ktfs* ktfs, struct ktfs_inode* inode, uint32_t offset, void* buf, uint32_t len);

// Forward declarations for UIO callbacks 
static void ktfs_close(struct uio* uio);
static long ktfs_fetch(struct uio* uio, void* buf, unsigned long len);
static int ktfs_cntl(struct uio* uio, int cmd, void* arg);

// Fwd declarations for filesystem-level callbacks 
static int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr);
static int ktfs_create(struct filesystem* fs, const char* name);
static int ktfs_delete(struct filesystem* fs, const char* name);
static void ktfs_flush(struct filesystem* fs);

// UIO interface functions
static const struct uio_intf ktfs_file_intf = {
    .close = &ktfs_close,
    .read = &ktfs_fetch,
    .write = NULL,  // Read-only for CP1
    .cntl = &ktfs_cntl
};

// EXPORTED FUNCTION DEFINITIONS
//

// Mounts the file system with associated backing cache
int mount_ktfs(const char* name, struct cache* cache) {
    struct ktfs* ktfs;
    int result;
    
    if (name == NULL || cache == NULL) {
        return -EINVAL;
    }
    
    // Allocate KTFS struct
    ktfs = kcalloc(1, sizeof(struct ktfs));
    if (ktfs == NULL) {
        return -ENOMEM;
    }
    
    // Initialize KTFS
    ktfs->cache = cache;
    ktfs->num_open_files = 0;
    ktfs->open_files_head = NULL;
    lock_init(&ktfs->fs_lock);
    
    // Read and validate superblock
    result = read_superblock(ktfs);
    if (result != 0) {
        kfree(ktfs);
        return result;
    }
    
    // Validate superblock fields
    if (ktfs->superblock.block_count == 0 ||
        ktfs->superblock.inode_block_count == 0) {
        kfree(ktfs);
        return -EINVAL;
    }
    
    // Initialize the embedded filesystem interface and attach to VFS
    ktfs->fs.open = &ktfs_open;
    ktfs->fs.create = &ktfs_create;
    ktfs->fs.delete = &ktfs_delete;
    ktfs->fs.flush = &ktfs_flush;

    result = attach_filesystem(name, &ktfs->fs);
    if (result != 0) {
        kfree(ktfs);
        return result;
    }
    
    return 0;
}

// Opens a file with the given name
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) {
    struct ktfs* ktfs = (struct ktfs*)fs;
    struct ktfs_file* file;
    struct ktfs_dir_entry dentry;
    int result;
    
    if (fs == NULL || name == NULL || uioptr == NULL) {
        return -EINVAL;
    }
    
    lock_acquire(&ktfs->fs_lock);
    
    // Find the file in the directory
    result = find_file_in_directory(ktfs, name, &dentry);
    if (result != 0) {
        lock_release(&ktfs->fs_lock);
        return result;
    }
    
    // Allocate file structure
    file = kcalloc(1, sizeof(struct ktfs_file));
    if (file == NULL) {
        lock_release(&ktfs->fs_lock);
        return -ENOMEM;
    }
    
    // Initialize file structure
    file->fs = fs;
    memcpy(&file->dentry, &dentry, sizeof(struct ktfs_dir_entry));
    file->pos = 0;
    file->next = NULL;
    
    // Read the inode
    result = read_inode(ktfs, dentry.inode, &file->inode);
    if (result != 0) {
        kfree(file);
        lock_release(&ktfs->fs_lock);
        return result;
    }
    
    file->size = file->inode.size;
    
    // Initialize UIO interface
    uio_init1(&file->uio, &ktfs_file_intf);
    
    // Add to open files list (at the head)
    file->next = ktfs->open_files_head;
    ktfs->open_files_head = file;
    ktfs->num_open_files++;
    
    *uioptr = &file->uio;
    
    lock_release(&ktfs->fs_lock);
    return 0;
}

// Closes the file that is represented by the uio struct
void ktfs_close(struct uio* uio) {
    struct ktfs_file* file;
    struct ktfs* ktfs;
    
    if (uio == NULL) return;
    
    // Get file from UIO (uio is first member of ktfs_file)
    file = (struct ktfs_file*)uio;
    ktfs = (struct ktfs*)file->fs;
    
    lock_acquire(&ktfs->fs_lock);
    
    // Remove from open files list
    struct ktfs_file** curr = &ktfs->open_files_head;
    while (*curr) {
        if (*curr == file) {
            *curr = file->next; // Unlink
            ktfs->num_open_files--;
            break;
        }
        curr = &(*curr)->next;
    }
    
    lock_release(&ktfs->fs_lock);
    
    // Free the file structure
    kfree(file);
}

// Reads data from file attached to uio into provided argument buffer
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len) {
    struct ktfs_file* file;
    struct ktfs* ktfs;
    uint32_t bytes_to_read;
    int result;
    
    if (uio == NULL || buf == NULL) {
        return -EINVAL;
    }
    
    file = (struct ktfs_file*)uio;
    ktfs = (struct ktfs*)file->fs;
    
    lock_acquire(&ktfs->fs_lock);
    
    // Calculate how many bytes to read
    if (file->pos >= file->size) {
        lock_release(&ktfs->fs_lock);
        return 0; // EOF
    }
    
    bytes_to_read = len;
    if (file->pos + bytes_to_read > file->size) {
        bytes_to_read = file->size - file->pos;
    }
    
    // Read data
    result = read_file_data(ktfs, &file->inode, file->pos, buf, bytes_to_read);
    if (result < 0) {
        lock_release(&ktfs->fs_lock);
        return result;
    }
    
    // Update file pos
    file->pos += bytes_to_read;
    
    lock_release(&ktfs->fs_lock);
    return bytes_to_read;
}

// Execute control functions for the file
int ktfs_cntl(struct uio* uio, int cmd, void* arg) {
    struct ktfs_file* file;
    struct ktfs* ktfs;
    
    if (uio == NULL) {
        return -EINVAL;
    }
    
    file = (struct ktfs_file*)uio;
    ktfs = (struct ktfs*)file->fs;
    
    lock_acquire(&ktfs->fs_lock);
    
    if (cmd == FCNTL_GETEND) {
        if (arg == NULL) {
            lock_release(&ktfs->fs_lock);
            return -EINVAL;
        }
        *(unsigned long long*)arg = file->size;
        lock_release(&ktfs->fs_lock);
        return 0;
    } else if (cmd == FCNTL_GETPOS) {
        if (arg == NULL) {
            lock_release(&ktfs->fs_lock);
            return -EINVAL;
        }
        *(unsigned long long*)arg = file->pos;
        lock_release(&ktfs->fs_lock);
        return 0;
    } else if (cmd == FCNTL_SETPOS) {
        if (arg == NULL) {
            lock_release(&ktfs->fs_lock);
            return -EINVAL;
        }
        unsigned long long new_pos = *(unsigned long long*)arg;
        if (new_pos > file->size) {
            lock_release(&ktfs->fs_lock);
            return -EINVAL; // Can't seek past end in read-only fs
        }
        file->pos = (uint32_t)new_pos;
        lock_release(&ktfs->fs_lock);
        return 0;
    } else if (cmd == FCNTL_MMAP) {
        kprintf("MMAP is not supported yet\n");
        lock_release(&ktfs->fs_lock);
        return -ENOTSUP;
    } else {
        lock_release(&ktfs->fs_lock);
        return -ENOTSUP;
    }
}

// Flushes the cache to the backing device
void ktfs_flush(struct filesystem* fs) {
    struct ktfs* ktfs;
    
    if (fs == NULL) return;
    
    ktfs = (struct ktfs*)fs;
    
    lock_acquire(&ktfs->fs_lock);
    
    // Flush the cache
    if (ktfs->cache != NULL) {
        cache_flush(ktfs->cache);
    }
    
    lock_release(&ktfs->fs_lock);
}

// INTERNAL HELPER FUNCTIONS
//

// Read the superblock from the filesystem
static int read_superblock(struct ktfs* ktfs) {
    void* block;
    int result;
    
    // Get block 0 (superblock)
    result = cache_get_block(ktfs->cache, 0, &block);
    
    if (result != 0) {
        return result;
    }
    
    // Copy superblock data
     memcpy(&ktfs->superblock, block, sizeof(struct ktfs_superblock));
    
    // Release block
    cache_release_block(ktfs->cache, block, 0);
    
    return 0;
}

// Read an inode from the filesystem
static int read_inode(struct ktfs* ktfs, uint16_t inode_num, struct ktfs_inode* inode) {
    void* block;
    uint32_t block_num;
    uint32_t inode_offset;
    int result;
    
    // Calculate which block contains this inode
    uint32_t inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ;
    uint32_t inode_block_idx = inode_num / inodes_per_block;
    uint32_t inode_in_block = inode_num % inodes_per_block;
    
    // Calculate actual block number
    block_num = 1 + ktfs->superblock.inode_bitmap_block_count + 
                ktfs->superblock.bitmap_block_count + inode_block_idx; // off by one error maybe
    
    // Get the block
    result = cache_get_block(ktfs->cache, block_num * KTFS_BLKSZ, &block);
    
    if (result != 0) {
        return result;
    }
    
    // Copy inode data
    inode_offset = inode_in_block * KTFS_INOSZ;
    memcpy(inode, (char*)block + inode_offset, sizeof(struct ktfs_inode));
    
    // Release block
    cache_release_block(ktfs->cache, block, 0);
    
    return 0;
}

// Find a file in the root directory
static int find_file_in_directory(struct ktfs* ktfs, const char* name, struct ktfs_dir_entry* dentry) {
    struct ktfs_inode root_inode;
    uint32_t entries_per_block;
    uint32_t total_entries;
    uint32_t entries_read = 0;
    int result;
    
    // Read root directory inode
    result = read_inode(ktfs, ktfs->superblock.root_directory_inode, &root_inode);
    if (result != 0) {
        return result;
    }
    
    entries_per_block = KTFS_BLKSZ / KTFS_DENSZ;
    total_entries = root_inode.size / KTFS_DENSZ;
    
    // Search through directory entries
    while (entries_read < total_entries) {
        uint32_t block_idx = entries_read / entries_per_block;
        uint32_t entry_in_block = entries_read % entries_per_block;
        
        // Get the data block number for this directory block
        uint32_t block_num = get_data_block_number(ktfs, &root_inode, block_idx);
        
        // fixed: check for error from get_data_block_number
        if (block_num == BLOCK_NUM_ERROR) {
            return -EIO;
        }
        
        // block_num += ktfs->superblock.inode_bitmap_block_count + ktfs->superblock.bitmap_block_count + ktfs->superblock.inode_block_count + 1;
        
        // Read the block
        void* block;
        result = cache_get_block(ktfs->cache, block_num * KTFS_BLKSZ, &block);
        
        if (result != 0) {
            return result;
        }
        
        // Search entries in this block
        struct ktfs_dir_entry* entries = (struct ktfs_dir_entry*)block;
        uint32_t entries_in_this_block = entries_per_block;
        if (entries_read + entries_in_this_block > total_entries) {
            entries_in_this_block = total_entries - entries_read;
        }
        
        for (uint32_t i = entry_in_block; i < entries_in_this_block; i++) {
            if (entries[i].inode != 0 && strcmp(entries[i].name, name) == 0) {
                memcpy(dentry, &entries[i], sizeof(struct ktfs_dir_entry));
                cache_release_block(ktfs->cache, block, 0);
                return 0;
            }
        }
        
        cache_release_block(ktfs->cache, block, 0);
        entries_read += entries_in_this_block - entry_in_block;
    }
    
    return -ENOENT;
}

// Get the actual block number for a file's logical block
// Returns block number, 0 for sparse blocks, or BLOCK_NUM_ERROR on I/O error
static uint32_t get_data_block_number(struct ktfs* ktfs, struct ktfs_inode* inode, uint32_t file_block_idx) {
    uint32_t ptrs_per_block = KTFS_BLKSZ / sizeof(uint32_t);
    void* block;
    uint32_t block_num;
    uint32_t offset = 0;
    int result;

    //quick fix
    // file_block_idx += ktfs->superblock.inode_bitmap_block_count + ktfs->superblock.bitmap_block_count + ktfs->superblock.inode_block_count + 1;
    offset += ktfs->superblock.inode_bitmap_block_count + ktfs->superblock.bitmap_block_count + ktfs->superblock.inode_block_count + 1;
    
    // Direct blocks
    if (file_block_idx < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        return inode->block[file_block_idx] + offset;
    }
    
    file_block_idx -= KTFS_NUM_DIRECT_DATA_BLOCKS;
    
    // Indirect block (L1)
    if (file_block_idx < ptrs_per_block) {
        // if (inode->indirect == 0) return 0; // is this right?
        
        result = cache_get_block(ktfs->cache, inode->indirect * KTFS_BLKSZ, &block); // block is our inode
        
        if (result != 0) return BLOCK_NUM_ERROR; 
        
        block_num = ((uint32_t*)block)[file_block_idx] + offset;
        cache_release_block(ktfs->cache, block, 0);
        return block_num; // Returns a data block number
    }
    
    file_block_idx -= ptrs_per_block;
    
    // Doubly-indirect blocks (L1 and L2)
    for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++) {
        if (file_block_idx < ptrs_per_block * ptrs_per_block) {
            // if (inode->dindirect[i] == 0) return 0; // dont thing this is right
            
            // Read doubly-indirect block
            result = cache_get_block(ktfs->cache, inode->dindirect[i] * KTFS_BLKSZ, &block); // block is first indirect inode
            
            if (result != 0) return BLOCK_NUM_ERROR; 
            
            uint32_t indirect_idx = file_block_idx / ptrs_per_block;
            uint32_t indirect_block_num = ((uint32_t*)block)[indirect_idx];
            cache_release_block(ktfs->cache, block, 0);
            
            if (indirect_block_num == 0) return 0;
            
            // fix: again offset to physical.
            uint32_t physical_indirect_block = indirect_block_num + data_block_offset;
            result = cache_get_block(ktfs->cache, physical_indirect_block * KTFS_BLKSZ, &block);
            
            if (result != 0) return BLOCK_NUM_ERROR; 
            
            uint32_t data_idx = file_block_idx % ptrs_per_block;
            block_num = ((uint32_t*)block)[data_idx] + offset;
            cache_release_block(ktfs->cache, block + offset, 0);
            return block_num;
        }
        file_block_idx -= ptrs_per_block * ptrs_per_block;
    }
    
    return 0; 
}

static int read_file_data(struct ktfs* ktfs, struct ktfs_inode* inode, uint32_t offset, void* buf, uint32_t len) {
    uint32_t bytes_read = 0;
    
    // absolute block# offset against the start of Data Block 
    uint32_t data_block_offset = 1 + ktfs->superblock.inode_bitmap_block_count +
                                     ktfs->superblock.bitmap_block_count +
                                     ktfs->superblock.inode_block_count;
    
    while (bytes_read < len) {
        uint32_t block_offset = (offset + bytes_read) % KTFS_BLKSZ;
        uint32_t file_block_idx = (offset + bytes_read) / KTFS_BLKSZ;
        uint32_t bytes_to_copy = KTFS_BLKSZ - block_offset;
        
        if (bytes_to_copy > len - bytes_read) {
            bytes_to_copy = len - bytes_read;
        }
        
        // Get the data block number!
        uint32_t block_num = get_data_block_number(ktfs, inode, file_block_idx);
        
        if (block_num == BLOCK_NUM_ERROR) {
            return -EIO;
        }
        
        if (block_num == 0) {
            // Sparse file - fill with zeros
            memset((char*)buf + bytes_read, 0, bytes_to_copy);
        } else {
            // Read from the block
            void* block;
            // block_num =
            int result = cache_get_block(ktfs->cache, block_num * KTFS_BLKSZ, &block);
            
            if (result != 0) {
                return result;
            }
            
            memcpy((char*)buf + bytes_read, (char*)block + block_offset, bytes_to_copy);
            cache_release_block(ktfs->cache, block, 0);
        }
        
        bytes_read += bytes_to_copy;
    }
    
    return bytes_read;
}

// Placeholder functions for CP2/CP3
int ktfs_create(struct filesystem* fs, const char* name) {
    return -ENOTSUP; // Not supported in read-only filesystem
}

int ktfs_delete(struct filesystem* fs, const char* name) {
    return -ENOTSUP; // Not supported in read-only filesystem
}

long ktfs_store(struct uio* uio, const void* buf, unsigned long len) {
    return -ENOTSUP; // Not supported in read-only filesystem
}

void ktfs_listing_close(struct uio* uio) {
    // Placeholder for CP3
}

long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // Placeholder for CP3
    return -ENOTSUP;
}