/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌‍‌⁠​⁠⁠‌
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
    // Fill to fulfill spec
    struct uio_intf*            intf;                // use this as the uio for the ktfs and define the open using this! 
    uint64_t                    file_size;
    struct ktfs_dir_entry       dentry;
    uint64_t                    pos;
    struct ktfs_filesystem*     fs;
    struct ktfs_file*           next;               // next file in the linked list
};

struct ktfs_file_list {
    struct ktfs_file* head;
    struct ktfs_file* tail;
};

static struct ktfs_file_list open_file_list = {
    .head = NULL,
    .tail = NULL
};


// ktfs_io or file io used oh wait i see, dont use this
// struct storage_uio {
//     struct uio base;
//     struct storage *sto;
//     unsigned long pos;
//     char *buffer;
// };


/*

Look at fsimpl.h!!!!

Need the filesystem struct perhaps because differnet types of file systems have DIFFERENT WAYS to open files and etc

Filesystem layer is responsibel for finding and OPENING files (given a path)

The UIO layer is responsible for reading/writing to already open files!!!!!!!

*/
/// KTFS File System struct
struct ktfs_filesystem {
    struct filesystem intf; 
    struct cache* backing_cache;        //   underlying cache
    
    struct ktfs_superblock sb;

    struct ktfs_inode root_inode;      // root directory inode

    struct ktfs_filesystem* next;       // pointer to next filesystem in filesystem list
};

// List of file systems

// Definition
struct ktfs_filesystem_list {
    struct ktfs_filesystem* head;
    struct ktfs_filesystem* tail;
};

static struct ktfs_filesystem_list fs_list = {
    .head = NULL,
    .tail = NULL
};

// our ktfs uio interface
static const struct uio_intf ktfs_uio_intf = {
    .close = ktfs_close,
    .read  = ktfs_fetch,
    .write = ktfs_store,
    .cntl  = ktfs_cntl
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

/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */
int mount_ktfs(const char* name, struct cache* cache) {
    // FIXME: No :(

    struct ktfs_filesystem* fs = (struct ktfs_filesystem*)kmalloc(sizeof(struct ktfs_filesystem));
    fs->backing_cache = cache;

    // need to read superblock?
    uint8_t* sb_ptr;
    cache_get_block(cache, 0, &sb_ptr);    // request superblock from underlying hardware
    struct ktfs_superblock* disk_sb = (struct ktfs_superblock*) sb_ptr;
    fs->sb = *disk_sb; 
    

    // set root of inode pointer??
    uint32_t inode_table_start_block = 1 + fs->sb.inode_bitmap_block_count + fs->sb.bitmap_block_count; // superblock is block 0

    // get the inode block 0
    uint8_t* inode_block_ptr;
    cache_get_block(cache, inode_table_start_block, &inode_block_ptr);
    fs->root_inode = *((struct ktfs_inode*) inode_block_ptr);               // first inode is root directory inode

    // intiailize the filesystem itnterface
    fs->intf.open = ktfs_open;
    fs->intf.create = ktfs_create;
    fs->intf.delete = ktfs_delete;
    fs->intf.flush = ktfs_flush;


    // Add to file system list
    if (!fs_list.head)
    {
        fs_list.head = fs;
        fs_list.tail = fs;
        fs->next = NULL;
    }
    else
    {
        fs_list.tail->next = fs;
        fs_list.tail = fs;
        fs->next = NULL;
    }

    attach_filesystem(name, fs);            // Mounts the actual file system (purely mounting)
    return -ENOTSUP;
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
    // FIXME
    /*
    Model???
    
    ktfs_file first member is ktfs_uio. Now we initialize that!
    
    
    */


    // SET THE UIOPTR TO BE THE UIO FROMTHE KTFS FILE!!!!!!
    // assume that the fs passed in is the KTFS (since it most lekly is)
    struct ktfs_filesystem* ktfs_fs = (struct ktfs_filesystem*) fs;             // since it will be first member always
    
    // get the superblock
    struct ktfs_superblock sb = ktfs_fs->sb;
    uint32_t inode_table_start_block = 1 + sb.inode_bitmap_block_count + sb.bitmap_block_count; // superblock is block 0
    // We know the denstry size from DEN_SIZE macro
    uint32_t inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ;
    uint32_t dentries_per_block = KTFS_BLKSZ / KTFS_DENSZ;
    
    // number of inodes in use/in the data block of dentries
    uint32_t total_dentries = ktfs_fs->root_inode.size / KTFS_DENSZ; // root directory inode size holds number of dentries




    // loop through all dentries to find
    for (int i = 0; i < total_dentries; i++)
    {
        uint32_t dentry_block_index = i / dentries_per_block;
        uint32_t dentry_index_in_block = i % dentries_per_block;
        uint8_t* data_block;    // block of dentries

        // either in direct or indirect

        // get the data block for the dentry

        // direct
        if (dentry_block_index < KTFS_NUM_DIRECT_DATA_BLOCKS)
        {
            cache_get_block(ktfs_fs->backing_cache, ktfs_fs->root_inode.block[dentry_block_index], &data_block);
        }
        else if (dentry_block_index < KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ / sizeof(uint32_t)))
        {
            // indirect
            uint32_t indirect_index = dentry_block_index - KTFS_NUM_DIRECT_DATA_BLOCKS;                 // indirect index
            uint8_t* indirect_block;
            cache_get_block(ktfs_fs->backing_cache, ktfs_fs->root_inode.indirect, &indirect_block);     // get the indirect block
            uint32_t* indirect_block_ptrs = (uint32_t*) indirect_block;                                 // each indirect block "entry" is 4 bytes which is the index of actual block
            uint32_t data_block_index = indirect_block_ptrs[indirect_index];                            // use indirect index to get where the datablock actually is
            cache_get_block(ktfs_fs->backing_cache, data_block_index, &data_block);                     // get the actual data block
        }
        else
        {
            // doubly indirect
            // number of pointers we have per block
            uint32_t PTRS_PER_BLOCK = KTFS_BLKSZ / sizeof(uint32_t);
            uint32_t base = KTFS_NUM_DIRECT_DATA_BLOCKS + PTRS_PER_BLOCK;

            // relative index within the double-indirect range
            uint32_t raw_index = dentry_block_index - base;

            // indices within each level
            uint32_t dindirect_index = raw_index / (PTRS_PER_BLOCK * PTRS_PER_BLOCK);       // doubly indirect index
            uint32_t indirect_index = (raw_index / PTRS_PER_BLOCK) % PTRS_PER_BLOCK;        // indirect index. Need to mod since it wraps
            uint32_t direct_index = raw_index % PTRS_PER_BLOCK;                               // direct index
            

            uint8_t* dindirect_block;
            cache_get_block(ktfs_fs->backing_cache, ktfs_fs->root_inode.dindirect[dindirect_index], &dindirect_block);
            uint32_t* indirect_ptrs = (uint32_t*) dindirect_block;

            uint8_t* indirect_block;
            cache_get_block(ktfs_fs->backing_cache, indirect_ptrs[indirect_index], &indirect_block);
            uint32_t* data_ptrs = (uint32_t*) indirect_block;

            uint32_t data_block_index = data_ptrs[direct_index];
            
            cache_get_block(ktfs_fs->backing_cache, data_block_index, &data_block);
        }
        struct ktfs_dir_entry* dentry_ptr = (struct ktfs_dir_entry*)(data_block + (dentry_index_in_block * KTFS_DENSZ));    // locate the specific dentry
        // found the file
        if (strcmp(dentry_ptr->name, name) == 0)
        {
            // get the Inode for the file
            uint16_t inode_number = dentry_ptr->inode;
            uint32_t inode_block_index = inode_number / inodes_per_block;
            uint32_t inode_index_in_block = inode_number % inodes_per_block;
            uint8_t* inode_block;
            cache_get_block(ktfs_fs->backing_cache, inode_table_start_block + sb.inode_block_count + inode_block_index, &inode_block);
            struct ktfs_inode* inode_ptr = (struct ktfs_inode*)(inode_block + (inode_index_in_block * KTFS_INOSZ));
             // we assume one file system, so just use fs_list.head
            struct ktfs_file* open_file = kmalloc(sizeof(open_file));   //  create new open file
            uio_init0((struct uio*) open_file, &ktfs_uio_intf);                   //   setup uio interface for the file, also uio is first member of this
            open_file->file_size = inode_ptr->size;
            open_file->fs = ktfs_fs;
            open_file->pos = 0;
            if (open_file_list.head == NULL)
            {
                open_file_list.head = open_file;
                open_file_list.tail = open_file;
            }
            else
            {
                open_file_list.tail->next = open_file;
                open_file_list.tail = open_file;
            }
            open_file->next = NULL;
        }
    }


    return -ENOTSUP;
}

/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */
void ktfs_close(struct uio* uio) {
    // FIXME
    // THE IO POINTER BASED UPON ABOVE STRUCT IS THE FIRST!!!!!!!!!!!!! SO WE CAN JUST GET THE STRUCT HERE!!!!!!
    return;
}

/**
 * @brief Reads data from file attached to uio into provided argument buffer
 * @param uio uio of file to be read
 * @param buf Buffer to be filled
 * @param len Number of bytes to read
 * @return Number of bytes read if successful, negative error code if error
 */
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len) {
    // FIXME
    return -ENOTSUP;
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
    // FIXME
    return -ENOTSUP;
}

/**
 * @brief Flushes the cache to the backing device
 * @return 0 if flush successful, negative error code if error
 */
void ktfs_flush(struct filesystem* fs) {
    // FIXME
    return;
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
