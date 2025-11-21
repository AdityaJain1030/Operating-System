/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‌‌​‌‍‍⁠​⁠⁠‌
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/  

//todo list
/*
 * ask TA about nested mounts and multiple mountpoints
 * ask TA about negative return when no file is found on ktfs_open
 * figure out passing in NULL double pointers. If it isn't ok then change it everywhere and adjust guardcases
*/

#include <stddef.h>
#include <stdint.h>
#include <string.h>
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

//notably, store is the most normal case. 
//create operates on the root directory inode, which needs to be fetched from the backing device, (pass NULL for inode)
//and setend the operation into a noop/memset0 (haven't decided which one yet) on the file of choice (pass NULL for buf and bytecnt)
#define F_APPEND_STORE 0
#define F_APPEND_CREATE 1
#define F_APPEND_SETEND 2

// INTERNAL TYPE DEFINITIONS
//

struct ktfs {
    struct filesystem fs;
    //superblock data. provided in the makefile from the beginning
    uint32_t block_cnt; //new addition!!!!
    uint32_t inode_bitmap_block_start; 
    uint32_t bitmap_block_start;
    uint32_t inode_block_start;
    uint32_t data_block_start;
    uint16_t root_directory_inode;
    //uint16_t reserved;
    int16_t reserved[3];
    //tells us if it's already been mounted or nah
    //int mounted;

    //post cp1 change: fs moved to the top because offset of was acting up in create and delete
    struct cache * cache_ptr;

    int max_inode_count;
    //int num_files; post cp1 change: this is a redundancy

    struct ktfs_inode root_directory_inode_data; //we always update this when we change the root directory inode
};


/// @brief File struct for a file in the Keegan Teal Filesystem
struct ktfs_file {
    struct uio base;

    struct ktfs_dir_entry dentry; //scanned dentry data of the file (in mount). note that if the file is deleted, the whole struct gets freed anyway
    
    int opened; 
    uint32_t pos; // Position in the current opened file
    struct ktfs_inode inode_data; // we fill out the inode data when we open the file. when we close the file, free it and set the pointer to null. 
};

struct ktfs_file_records{
    uint64_t reserved;
    struct ktfs_file * filetab[];
};



struct ktfs_file_records * records; //our singular book-keeping system for files

struct ktfs * ktfs; // Changed to a global, bc we only have one ktfs



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

int ktfs_get_block_absolute_idx(struct cache* cache, struct ktfs_inode* inode, uint32_t contiguous_db_index);
int ktfs_appender(struct cache* cache, struct ktfs_inode* inode, void * buf, int bytecnt, int op);
int ktfs_alloc_datablock(struct cache* cache, struct ktfs_inode * inode, uint32_t contigous_db_idx_to_add);
int ktfs_find_and_use_free_db_slot(struct cache* cache);
int ktfs_find_and_use_free_inode_slot(struct cache* cache);
int ktfs_free_db_slot(struct cache* cache, uint32_t db_blk_num);//
int ktfs_free_inode_slot(struct cache* cache, uint32_t inode_slot_num); //



static const struct uio_intf initial_file_uio_intf = {
    .close = &ktfs_close,
    .cntl = &ktfs_cntl,
    .read = &ktfs_fetch,
    .write = &ktfs_store    
};


//exactly what it sounds like
//NOTE: db_blk_num is the DATA_BLOCK_INDEX of the block we're trying to free, not the absolute index like we've so often used for other function
int ktfs_free_db_slot(struct cache* cache, uint32_t db_blk_num){
    void * blkptr;
    cache_get_block(cache, (db_blk_num/(KTFS_BLKSZ*sizeof(uint8_t))+ktfs->bitmap_block_start)*KTFS_BLKSZ, &blkptr);//its a bitmap u dingus
    ((struct ktfs_data_block*)blkptr)->data[(db_blk_num%(KTFS_BLKSZ*sizeof(uint8_t)))/sizeof(uint8_t)] |= 0x01<<(db_blk_num%sizeof(uint8_t));
    cache_release_block(cache, blkptr, 1);
    return 0;
}

//exactly what it sounds like
//NOTE: inode_slot_num is the INODE index of the INODE we're trying to free, not the absolute index like we've so often used for other function
int ktfs_free_inode_slot(struct cache* cache, uint32_t inode_slot_num){
    void * blkptr;
    cache_get_block(cache, (inode_slot_num/(KTFS_BLKSZ*sizeof(uint8_t))+ktfs->bitmap_block_start)*KTFS_BLKSZ, &blkptr);//its a bitmap u dingus
    ((struct ktfs_data_block*)blkptr)->data[(inode_slot_num%(KTFS_BLKSZ*sizeof(uint8_t)))/sizeof(uint8_t)] |= 0x01<<(inode_slot_num%sizeof(uint8_t));
    cache_release_block(cache, blkptr, 1);
    return 0;
}

/*
* @ brief: starting from the end of an inodes data (SPECIFICALLY does not deal with data before the end of the file), WRITES more data to the inode while also allocating more dentry blocks where needed
* @ parameters: backing cache pointer, inode to extend, and number of bytes to extend. if we wish to operate on the root directory inode, pass NULL as the inode and 
* @ returns: returns the number of bytes that were written, or a negative number if an error occurred
*
* @ NOTE TO CALLER: DO NOOOOOT TRY TO USE THIS FUNCTION FOR AN INODE THAT ISN'T EITHER INSIDE OF A KTFS_FILE STRUCT OR
*                   A ROOT DIRECTORY INODE. PART OF THIS FUNCTION UTILIZES OFFSET_OF TO GET ACCESS TO THE WRAPPING KTFS_FILE STRUCT
* @ ANOTHER NOTE TO CALLER: MAKE SURE THE ROOT DIRECTORY INODE IS DYNAMICALLY ALLOCATED ATLEAST FOR THE DURATION OF THIS FUNCTION! I DO NOT WANT THAT SHIT ON THE STACK
*/
int ktfs_appender(struct cache* cache, struct ktfs_inode* inode, void * buf, int bytecnt, int op){ 
    trace("%s(cache=%p, inode=%p, bytecnt=%u)\n", __func__, cache, inode, bytecnt);
    struct ktfs_file * file_wrapper;
    uint32_t * pos;
    uint16_t inode_num; //specifically the number of the inode (we already have the root directory inode number as well)

    //guard cases
    if (bytecnt ==0) return 0;
    if (op != F_APPEND_SETEND && op != F_APPEND_STORE && op != F_APPEND_CREATE) return -ENOTSUP;


    if (op == F_APPEND_SETEND || op == F_APPEND_STORE){
        file_wrapper = (void *)inode - offsetof(struct ktfs_file, inode_data);
        if (file_wrapper->pos != inode->size) return -ENOTSUP; //Dawg I just told you, this function can only be called if we're at the end of the file
        pos = &file_wrapper->pos;
        inode_num = file_wrapper->dentry.inode;
    }
    else if (op==F_APPEND_CREATE){
        inode = &ktfs->root_directory_inode_data; //no filewrapper
        //pos is always "end of file" so to speak for the root directory inode
        pos = &ktfs->root_directory_inode_data.size;
        trace("size in appender: %d\n",ktfs->root_directory_inode_data.size);
        inode_num = ktfs->root_directory_inode;
        trace("inode number: %d\n", inode_num);
    }


    //ensures that we don't somehow exceed the maximum filesize
    bytecnt = MIN(bytecnt, KTFS_MAX_FILE_SIZE - *pos);


    int nstored = 0;
    void * blkptr;
    while (nstored < bytecnt){
        trace("*pos = %d\n", *pos);
        int allocated_index = -1;
        if (*pos % KTFS_BLKSZ == 0) {
            trace("*pos = %d\n", *pos);
            allocated_index = ktfs_alloc_datablock(cache, inode, *pos/KTFS_BLKSZ); //in ANY case where pos is exactly a multiple of blksz, that means we're at the beginning of a new block
            if (allocated_index <0){
                trace("alloc_datablock returned a negative value\n");
                return allocated_index;
            }//should always allocate in this case
        }
        if (allocated_index == -1) allocated_index = ktfs_get_block_absolute_idx(cache, inode, *pos/KTFS_BLKSZ); // alloc_index = -1 by the end of this that means we didn't have to allocate a new block for the operation
        int n_per_cycle = MIN(KTFS_BLKSZ, bytecnt - nstored);
        //actual store part
        if (cache_get_block(cache, allocated_index*KTFS_BLKSZ, &blkptr)<0){// (can make this a noop for setend and operations on the root directory inode, although that's a pretty trivial optimization)
            trace("cache_get_block returned a negative value\n");

            return -EINVAL;
        }
        if (op == F_APPEND_SETEND) memset(blkptr, 0, n_per_cycle);
        else memcpy(blkptr, buf+nstored, (size_t) n_per_cycle);
        cache_release_block(cache, blkptr, 1); //we wrote to a block so its dirty

        //increment values
        nstored += n_per_cycle;
        *pos += n_per_cycle;
        if (op != F_APPEND_CREATE) inode->size += n_per_cycle; //because for "create" there is no filewrapper, and the pos itsself points to size

    }

    //cache_get_block(ktfs_inst->cache_ptr, (ktfs_inst->root_directory_inode/KTFS_NUM_INODES_IN_BLOCK), &blkptr);
    //struct ktfs_inode* rdr = (struct ktfs_inode*)blkptr + (ktfs->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK);
    //lmao I was so sleep deprived that I forgot that I accounted for this, and then designed around this not happening
    //
    //in this current impementation, we perform a DEEP COPY so that the file metadata persists in the filesystem
    //we do this at the end because doing in every loop would be abysmal TODO: maybe it is required in every loop though
    trace("inode_num: %d\n", inode_num);
    if (cache_get_block(cache, ((inode_num/KTFS_NUM_INODES_IN_BLOCK)+ktfs->inode_block_start)*KTFS_BLKSZ, &blkptr)< 0){
        trace("cache_get_block returned a negative value\n");
        return -EINVAL;
    }
    memcpy((char *)blkptr +inode_num*KTFS_INOSZ, inode, KTFS_INOSZ);//memcpy the inode back into the inode blocks at the correct position (given by inode_num% KTFS_NUM_INODES_IN_BLOCK
    cache_release_block(cache, blkptr, 1);

    return nstored;
}


/*
* @ brief: 
* @ parameters: 
* @ return: (for convience) the absolute index of the 
* NOTE: THIS DOESN"T HAVE ANY SIDE EFFECT ON THE FILE SIZE
*/
int ktfs_alloc_datablock(struct cache* cache, struct ktfs_inode * inode, uint32_t contiguous_db_to_alloc){
    kprintf("%s(cache=%p, inode=%p, contiguous_db_to_alloc=%u)\n", __func__, cache, inode, contiguous_db_to_alloc);
    int alloc_db_idx;
    void * blkptr;

    //one by one checks needs for direct, indirect, and dindirect allocation
    
    
    if (contiguous_db_to_alloc < KTFS_NUM_DIRECT_DATA_BLOCKS){
        alloc_db_idx = ktfs_find_and_use_free_db_slot(cache);
        //kprintf("return from findand use free_db_slot funciton\n");
        if (alloc_db_idx <0) return alloc_db_idx;
        inode->block[contiguous_db_to_alloc] = alloc_db_idx;  
        return alloc_db_idx + ktfs->data_block_start;
    }
    contiguous_db_to_alloc -= KTFS_NUM_DIRECT_DATA_BLOCKS;


    if (contiguous_db_to_alloc< 128){
    
        if (contiguous_db_to_alloc == 0){ //case that its the first block we need to store in the indirects, so there isn't already a datablock indirection
            alloc_db_idx = ktfs_find_and_use_free_db_slot(cache);
            if (alloc_db_idx <0)return alloc_db_idx;
            inode->indirect = alloc_db_idx;
        }
        
        int new_indir_blk = ktfs_find_and_use_free_db_slot(cache);//allocate new indirect blk (leaf)
        if (new_indir_blk < 0){
            inode->indirect = 0;
            return new_indir_blk;
        }

        //write new pointer into the indirect blk
        if (cache_get_block(cache, (inode->indirect + ktfs->data_block_start) * KTFS_BLKSZ, &blkptr)< 0){
            trace("cache_get_block returned a negative value\n");
            return -EINVAL;
        }
        ((uint32_t*)blkptr)[contiguous_db_to_alloc] = new_indir_blk;
        cache_release_block(cache, blkptr, 1);
        
    }

    contiguous_db_to_alloc -= 128;

    //TODO: dindirect case


    return -ENOTSUP; 
}


//finds AND CLAIMS the first free datablock on the bitmap. also marks the slot as used before returning
//simply a helper function for ktfs_alloc_datablock, since I'd have to write the smae code over and over again for otherwise
//NOTE: RETURNS THE DATABLOCK IDX NOT ABSOLUTE IDX
int ktfs_find_and_use_free_db_slot(struct cache * cache){
    kprintf("%s(cache)", __func__);
    struct ktfs_bitmap bitmap;
    void * blkptr;

    //note that if this function returned a negative, the allocation should have ABSOLUTELY NEVER HAPPENED

    // int nbitmapblocks = ktfs->inode_block_start - ktfs->bitmap_block_start;
    // int currblk =0;
    // int found_flag = 0;
    // ^ this code is actually wrong because the number of bits we should scan is acutally the max number of data_blocks
    int n_db = (ktfs->block_cnt - ktfs->data_block_start)* KTFS_BLKSZ;
    int curr_db = 0;
    //mount_ktfs example from back when I was well rested
            // int absolute_idx = ktfs_get_block_absolute_idx(ktfs->cache_ptr, &ktfs->root_directory_inode_data, i/KTFS_NUM_DENTRY_IN_BLOCK); 
            // if (absolute_idx < 0) return absolute_idx;//propagate errors. more importantly this is the only function in mount that won't print an error for trace, so if it fails you know why
            // cache_get_block(ktfs->cache_ptr, absolute_idx*KTFS_BLKSZ, &tempptr);
            // if (retval <0){ 
            //     trace("cache_get_block failed\n");
            //     cache_release_block(ktfs->cache_ptr, (void *)superblock, 0);
            //     return retval;
            // } 
            // //trace("got block for dentry scan\n");
            // memcpy((void *)&dentry_block, tempptr, sizeof(dentry_block));
            // cache_release_block(ktfs->cache_ptr, tempptr, 0);
    // while (curr_db < n_db){
    //     if (curr_db % KTFS_BLKSZ ==0) {
    //         if (cache_get_block(cache, (ktfs->bitmap_block_start + curr_db/KTFS_BLKSZ)*KTFS_BLKSZ, &blkptr)<0) return -900; 
    //         //memcpy((void*)&bitmap, blkptr, sizeof(struct ktfs_bitmap));
    //
    //     }
    //     // if (bitmap->bytes[curr_db%KTFS_BLKSZ] ==0){
    //     //     bitmap->bytes[curr_db%KTFS_BLKSZ] = 1;
    //     //     cache_release_block(cache, blkptr, 1);
    //     //     return curr_db;
    //     // }
    //
    //     if (curr_db % KTFS_BLKSZ ==0) cache_release_block(cache, (void *)bitmap, 0); //accompanying release block in any case where we got one
    //     curr_db++;
    // }
    while(curr_db < n_db){
        int ndb_per_cycle = MIN(KTFS_BLKSZ, n_db - curr_db);
        if (cache_get_block(cache, (ktfs->bitmap_block_start + curr_db/KTFS_BLKSZ)*KTFS_BLKSZ, &blkptr)<0) return -900;
        
        for (int i = 0; i < ndb_per_cycle; i++){
            if(((char *)blkptr)[i] == 0){
                ((char *)blkptr)[i] = 1;
                cache_release_block(cache, blkptr, 1);
                return curr_db;
            }
            curr_db++;
        }
        cache_release_block(cache, blkptr, 0);

    }

    return -ENODATABLKS;
}


//finds AND CLAIMS the first free datablock on the bitmap. also marks the slot as used before returning
//simply a helper function for ktfs_alloc_datablock, since I'd have to write the smae code over and over again for otherwise
//NOTE: RETURNS THE DATABLOCK IDX NOT ABSOLUTE IDX
int ktfs_find_and_use_free_inode_slot(struct cache * cache){
    struct ktfs_bitmap bitmap;
    void * blkptr;

    //note that if this function returned a negative, the allocation should have ABSOLUTELY NEVER HAPPENED

    // int nbitmapblocks = ktfs->inode_block_start - ktfs->bitmap_block_start;
    // int currblk =0;
    // int found_flag = 0;
    // ^ this code is actually wrong because the number of bits we should scan is acutally the max number of data_blocks
    int n_ino = ktfs->max_inode_count;
    int curr_ino = 1; //start from 1 because 0 is the root directory inode

    while (curr_ino < n_ino){
        
        int n_ino_per_cycle = MIN(KTFS_BLKSZ, n_ino - curr_ino);
        if (cache_get_block(cache, (ktfs->bitmap_block_start + curr_ino/KTFS_BLKSZ)*KTFS_BLKSZ, &blkptr)<0) return -900;
        
        for (int i = 0; i < n_ino_per_cycle; i++){
            if(((char *)blkptr)[i] == 0){
                ((char *)blkptr)[i] = 1;
                cache_release_block(cache, blkptr, 1);
                return curr_ino;
            }
            curr_ino++;
        }
        cache_release_block(cache, blkptr, 0);
    }
    // while (curr_ino < n_ino){
    //     if (curr_ino % KTFS_BLKSZ ==0) {
    //         trace("pos:%d\n", ktfs->inode_bitmap_block_start + curr_ino/KTFS_NUM_INODES_IN_BLOCK);
    //         if (cache_get_block(cache, (ktfs->inode_bitmap_block_start + curr_ino/KTFS_NUM_INODES_IN_BLOCK)*KTFS_BLKSZ, blkptr)<0) return -900;
    //         memcpy((void *)&bitmap, blkptr, sizeof(struct ktfs_bitmap));
    //         cache_release_block(cache, blkptr, 0);
    //     }
    //     if (bitmap.bytes[curr_ino%KTFS_BLKSZ] ==0){
    //         bitmap.bytes[curr_ino%KTFS_BLKSZ] = 1;
    //
    //         return curr_ino;
    //     }

        //if (curr_ino % KTFS_BLKSZ ==0) cache_release_block(cache, (void *)bitmap, 0); //accompanying release block in any case where we got one
    //     curr_ino++;
    // }

    return -ENOINODEBLKS;
}

/*
* @ brief: returns the ABSOLUTE block index inside the backing device. where (0 is the index of the superblock)
* @ parameters: pointer to the backing cache struct pointer to the inode of the file we're trying to traverse, and the index of which one of the contiguous datablocks we are trying to access
* @ return: negative value on failure, ABSOLUTE block index of the datablock we were looking for at the 512 aligned position that we specified as contiguous_db_index arguement
*/
int ktfs_get_block_absolute_idx(struct cache* cache, struct ktfs_inode* inode, uint32_t contiguous_db_index){
    trace("%s(cache=%p, inode=%p, contiguous_db_index=%u)\n", __func__, cache, inode, contiguous_db_index);

    struct ktfs_data_block * indirect;
    struct ktfs_data_block * dindirect;
    int retval;


    //logic for returning the correct absolute block. 

    if (contiguous_db_index < KTFS_NUM_DIRECT_DATA_BLOCKS){
        return inode->block[contiguous_db_index] + ktfs->data_block_start;
    }

    contiguous_db_index -= KTFS_NUM_DIRECT_DATA_BLOCKS; 


    //there is 128 indirect indexes
    if (contiguous_db_index < 128){

        retval = cache_get_block(ktfs->cache_ptr, (inode->indirect+ktfs->data_block_start)*KTFS_BLKSZ, (void **)&indirect);
        if (retval < 0){ 
            trace("cache_get_block failed");
            cache_release_block(ktfs->cache_ptr, indirect, 0);
            return retval;
        }

        
        uint32_t indirect_index = ((uint32_t *)indirect)[contiguous_db_index];
        cache_release_block(ktfs->cache_ptr, indirect, 0);
        return indirect_index + ktfs->data_block_start;
    }

    contiguous_db_index -= 128;

    //dindirects now
    if (contiguous_db_index< 2*128*128){

        retval = cache_get_block(ktfs->cache_ptr, (inode->dindirect[contiguous_db_index/(128*128)]+ktfs->data_block_start)*KTFS_BLKSZ, (void **)&indirect);
        if (retval < 0){ 
            trace("ktfs_get_block_absolute_idx failed when trying to execute cache_get_block for a double indirect (first time)");
            cache_release_block(ktfs->cache_ptr,indirect, 0);
            return retval;
        }        

        uint32_t indirect_index = ((uint32_t *)indirect)[(contiguous_db_index%(128*128))/128];

        cache_release_block(ktfs->cache_ptr, indirect,0);

        retval = cache_get_block(ktfs->cache_ptr, (ktfs->data_block_start + indirect_index)*KTFS_BLKSZ, (void **)&dindirect);
        if (retval < 0){ 
            trace("ktfs_get_block_absolute_idx failed when trying to execute cache_get_block for a double indirect (second time)");
            cache_release_block(ktfs->cache_ptr, dindirect, 0);
            return retval;
        }
        
        uint32_t dindirect_index = ((uint32_t *)dindirect)[contiguous_db_index%128];
        cache_release_block(ktfs->cache_ptr, (void *)dindirect, 0);
        
        dindirect_index += ktfs->data_block_start; //makes it the absolute offset of the first layer of indirection

        return dindirect_index;

    }
    


        trace("ktfs_get_block_indirection_helper is cooked, should NOT have even reached this point") ;
        return -EINVAL ;

}

/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */
int mount_ktfs(const char* name, struct cache* cache) {
    trace("%s(name=%s, cache=%p)\n", __func__, name, cache);
    int retval;
   
    //my implementation only allows for one ktfs implementation (which is probably fine considering the minimal heap in CP2 and no way to handle double this information)
    //so if there is a ktfs allocated we return error because our one allowed instance has already been mounted. If not, we allocate it ad attach the cache
    if (ktfs != NULL) return -EINVAL; 
    ktfs = kcalloc(1, sizeof(struct ktfs)); 
    ktfs->cache_ptr = cache;  
    //FIXME: not really a fixme but maybe its a good idea to save pointing the ktfs instance to what we've allocated at the END of the functions
    //       in order to stop threads from trying to mount it adversarially


    //"get superblock values" section //
    struct ktfs_superblock * superblock;
    
    retval = cache_get_block(ktfs->cache_ptr, 0, (void **) &superblock);
    if (retval < 0){ 
        trace("cache_get_block failed\n");
        return retval;
    } 

    uint32_t sumval = 1; 
    ktfs->inode_bitmap_block_start = sumval; // inode_bitmap_block_start = 1

    trace("inode_bitmap_block_count = %d\n", superblock->inode_bitmap_block_count);
    sumval += superblock->inode_bitmap_block_count;
    ktfs->bitmap_block_start = sumval; // bitmap_block_start = inode_bitmap_block_count + 1

    trace("bitmap_block_count = %d\n", superblock->bitmap_block_count);
    sumval += superblock->bitmap_block_count;
    ktfs->inode_block_start = sumval; // inode_block_start = bitmap_block_count + inode_bitmap_block_count + 1
    
    trace("inode_block_count = %d\n", superblock->inode_block_count);
    sumval += superblock->inode_block_count;
    ktfs->data_block_start = sumval; // data_block_start = inode_block_count + bitmap_block_count + inode_bitmap_block_count + 1

    ktfs->root_directory_inode = superblock->root_directory_inode;
    ktfs->block_cnt = superblock->block_count; //new addition!!!!

    //allocates an array for the blank definition in records, as big as the max number of inodes (calculated by the number of bytes in the inode blocks, divided by the size of an inode) 
    ktfs->max_inode_count = (superblock->inode_block_count * KTFS_BLKSZ)/KTFS_INOSZ; 
    trace("max_inode_count at mount_ktfs: %d\n", ktfs->max_inode_count);
    records = kcalloc(1, sizeof(struct ktfs_file_records)+ktfs->max_inode_count* sizeof(uint64_t));
    cache_release_block(ktfs->cache_ptr, (void*)superblock, 0); 
    //end of "get superblock values" section // 
    
 
    //"initialize and attach filesystem" section //
    trace("pre kcalloc\n");
    //ktfs->fs = kcalloc(1, sizeof(struct filesystem)); //post cp1 change: fs member is no longer pointer 
    trace("post kcalloc\n");
    ktfs->fs.create = &ktfs_create;
    ktfs->fs.delete = &ktfs_delete;
    ktfs->fs.flush = &ktfs_flush;
    ktfs->fs.open =  &ktfs_open;


    if (attach_filesystem(name, &ktfs->fs)){//if attach_filesystem returns anything other than 0 it failed to mount
        trace("failed to attach filesystem (mountpoint already exists)\n");
        return -EEXIST;  
    }
    //end of "initialize and attach filesystem" section //


    //"find root directory inode because it might not be 0" section //
    int root_dir_inode_relative_offset = ktfs->root_directory_inode / KTFS_NUM_INODES_IN_BLOCK;
    int root_dir_inode_inblock_offset = ktfs->root_directory_inode % KTFS_NUM_INODES_IN_BLOCK;

    struct ktfs_data_block * block=NULL; 
    retval = cache_get_block( ktfs->cache_ptr , (ktfs->inode_block_start + root_dir_inode_relative_offset)*KTFS_BLKSZ, (void **) &block);  
    if (retval <0){ 
        trace("cache_get_block failed\n");
        return retval;
    }  
    memcpy(&ktfs->root_directory_inode_data, (char*)block+root_dir_inode_inblock_offset*KTFS_INOSZ, sizeof(struct ktfs_inode)); 
    cache_release_block(ktfs->cache_ptr, block, 0);
    //ktfs->num_files = ktfs->root_directory_inode_data.size / KTFS_DENSZ; 
    int num_files = ktfs->root_directory_inode_data.size/KTFS_DENSZ;

    /*
     SCANNING ALL DENTRIES AT THE BEGINNING
    motivation: scanning all of the dentries in the file system so that we have a full record book of all the files we start out with and their names. Decreases runtime tremendously compared to scanning through all the dentries on every file_open
    implementation: we traverse all of the dentries in the root_directory_inodes datablocks with the help of our absolute_idx function. we keep a whole datablock allocated onto the struct and memcpy the entire datablock onto it whenever we're done reading the last one
    */
    void * tempptr;
    struct ktfs_dir_entry dentry_block[KTFS_NUM_DENTRY_IN_BLOCK];
    for (int i = 0; i < num_files; i++){
        if ((i % KTFS_NUM_DENTRY_IN_BLOCK) == 0){ 
            int absolute_idx = ktfs_get_block_absolute_idx(ktfs->cache_ptr, &ktfs->root_directory_inode_data, i/KTFS_NUM_DENTRY_IN_BLOCK); 
            if (absolute_idx < 0) return absolute_idx;//propagate errors. more importantly this is the only function in mount that won't print an error for trace, so if it fails you know why
            cache_get_block(ktfs->cache_ptr, absolute_idx*KTFS_BLKSZ, &tempptr);
            if (retval <0){ 
                trace("cache_get_block failed\n");
                cache_release_block(ktfs->cache_ptr, (void *)superblock, 0);
                return retval;
            } 
            //trace("got block for dentry scan\n");
            memcpy((void *)&dentry_block, tempptr, sizeof(dentry_block));
            cache_release_block(ktfs->cache_ptr, tempptr, 0);

        }


        records->filetab[i] = kcalloc(1, sizeof(struct ktfs_file));

        memcpy(&records->filetab[i]->dentry,&dentry_block[i%KTFS_NUM_DENTRY_IN_BLOCK], KTFS_DENSZ); //immediately copy in the dentry data into the file record
        
        struct uio_intf * file_uio_intf = kcalloc(1, sizeof(struct uio_intf)); //immediately add a uio interface for all the files we scan through
        memcpy(file_uio_intf, &initial_file_uio_intf, sizeof(struct uio_intf));
        uio_init0(&records->filetab[i]->base, file_uio_intf);
        trace("name:%s, inode: %d\n", records->filetab[i]->dentry.name,records->filetab[i]->dentry.inode);
    }

    trace("successful ktfs mount\n");
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
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) { // we don't have to use fs pointer in this function
    trace("%s(fs=%p, name=%s, uioptr=%p)", __func__, fs, name, uioptr);
    int retval;
    void * blkptr;

    //preliminary guardcase check //
    if (!fs || !name) return -EINVAL; //the uioptr can be null right?

    if (strlen(name) > KTFS_MAX_FILENAME_LEN) return -EINVAL; //strlen doesn't count the null terminator so this guardcase is right. the strlen(name) should not exceed the maxlen

    if (!ktfs) return -EINVAL; //either the file mount hasn't happened or its in the process of happening

    //"search for inode that matches name" section //
    int i = 0;
    while (i < ktfs->max_inode_count){
        
        if (records->filetab[i] && !strncmp(name, records->filetab[i]->dentry.name, KTFS_MAX_FILENAME_LEN)){
            trace("name: %s, records->filetab[i]->dentry.name: %s\n",name, records->filetab[i]->dentry.name);
            break;
        }

        i++;
    }
    
    //guardcase for no matching file found 
    trace("i: %d\n",i);
    trace("max_inode_count = %d\n", ktfs->max_inode_count);
    if (i == ktfs->max_inode_count)return -ENOENT; //QUESTION: Do I return negative if nothing was found? 

    //guardcase for already opened 
    if (records->filetab[i]->opened ==1) return -EBUSY;

    //this wasn't nessesary because I already have an opened flag which is 1 whenever there is atleast one uioptr referencing it
    //guardcase for if the file is already referenced by another uioptr
    //if (uio_refcnt(&records->filetab[i]->base) >0) return -EBUSY; //it decrements to 0 anyway on close
    //else uio_addref() happens towards the end

    trace("found matching file name at index %d\n",i);


    //if i != records->max_inodes, then we have an i in bounds, which means a matching file name was found
    int inode_idx = records->filetab[i]->dentry.inode;
    int absolute_block_of_inode = inode_idx/KTFS_NUM_INODES_IN_BLOCK + ktfs->inode_block_start;
    int inter_block_inode_idx = inode_idx%KTFS_NUM_INODES_IN_BLOCK;
    
    //trace("size before memcpy in %s : %d\n", __func__, records->filetab[i]->inode_data.size);
    
    retval = cache_get_block(ktfs->cache_ptr, KTFS_BLKSZ*absolute_block_of_inode, &blkptr);
    if (retval < 0) {
        cache_release_block(ktfs->cache_ptr, blkptr, 0);
        return retval;
    }
    //struct ktfs_inode * inode_dest = kcalloc(1, sizeof(struct ktfs_inode));
    //struct ktfs_inode * inode_src = (struct ktfs_inode *)((struct ktfs_inode *)&blkptr+inter_block_inode_idx);//struct ktfs_inode * inode_src = (struct ktfs_inode *)*((struct ktfs_inode *)&blkptr+inter_block_inode_idx);
    struct ktfs_inode inode_src = ((struct ktfs_inode*)blkptr)[inter_block_inode_idx];
    memcpy(&records->filetab[i]->inode_data, &inode_src, sizeof(struct ktfs_inode));
    //records->filetab[i]->inode_data = inode_dest; //the inode_data pointer member now points to the corresponding inode
    trace("file starts at absolute position:%d\n", records->filetab[i]->inode_data.block[0]+ktfs->data_block_start);
    
    records->filetab[i]->opened = 1; // mark the file as open

    cache_release_block(ktfs->cache_ptr, blkptr, 0);
    //trace("size at end of %s : %d\n", __func__, records->filetab[i]->inode_data.size);
    
    //unsigned long pos = 0;
    //retval = uio_cntl(&records->filetab[i]->base, FCNTL_SETPOS, &pos); //set the position to 0 incase it was opened (and closed) before
    //if (retval< 0) return retval;

    records->filetab[i]->pos = 0;

    //trace("ktfs_filetab[%d]->inode. = %d\n", i, ktfs_filetab[i]->inode_data);

    *uioptr = &records->filetab[i]->base;
    uio_addref(*uioptr);
    return 0;

}

/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */
void ktfs_close(struct uio* uio) {
    trace("%s(uio=%p)", __func__, uio);
    struct ktfs_file * file = (void *)uio - offsetof(struct ktfs_file , base );
    file->pos = 0;
    file->opened = 0;
    //trace("%s: size: %d\n",__func__,file->inode_data.size);
    //we don't need to decrement the count here because uio_close (its wrapper function) already does that for us)
    //cache_flush??? //<- no. at this point the blocks are marked as clean or dirty in the cache. them being open or closed doesn't change anything about the cache behavior
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
    trace("%s(uio=%p, buf=%p, len=%u)", __func__, uio, buf, len);
    if (!uio || !buf) return -EINVAL;
    
    if (len == 0) return 0; //both the pointers were validated but the caller has for some reason requested to read 0 bytes
    
    struct ktfs_file * file = (void *)uio - offsetof(struct ktfs_file , base);
    int retval;

    //trace("reached here\n");//NOTE: FILE SIZE DOESN"T ACTUALLY ENCODE THE FILESIZE
    //uint32_t size = file->inode_data.size;
    //trace("%s: size: %d\n", __func__, size);
    
    if (!file->opened) return -EINVAL;

    if (file->pos >=file->inode_data.size) return 0; //return if we're already at the end of the file

    // Adjust len to read only up to the end of the file
    if (file->pos + len > file->inode_data.size) len = file->inode_data.size - file->pos; 


    unsigned long nfetched = 0;
    unsigned long nread;
    uint32_t absolute_idx;
    struct ktfs_data_block *cache_block;

    while (nfetched < len){
        
        nread = MIN(KTFS_BLKSZ - file->pos%KTFS_BLKSZ, len - nfetched); //chooses between the didtance btween the pos and the next block, or whatevers left to fetch
        
        absolute_idx = ktfs_get_block_absolute_idx(ktfs->cache_ptr, &file->inode_data, file->pos/KTFS_BLKSZ);
        if (absolute_idx < 0) return absolute_idx; //propagate error

        retval = cache_get_block(ktfs->cache_ptr, absolute_idx*KTFS_BLKSZ, (void *) &cache_block);
        if (retval < 0){
            trace("cache_get_block failed: retval: %d\n", retval);
            //cache_release_block(ktfs->cache_ptr, (void*)cache_block, 0);
            return retval;
        }
        memcpy((char *)buf+nfetched, (char *)(cache_block->data)+(file->pos % KTFS_BLKSZ),nread);
        cache_release_block(ktfs->cache_ptr, (void*)cache_block, 0);

        nfetched += nread;
        file->pos += nread;
    }
    return nfetched; 
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
    // Similar to fetch

    trace("%s(uio=%p, buf=%p, len=%u)\n", __func__, uio, buf, len);
    if (!uio || !buf) return -EINVAL;
    
    if (len == 0) return 0; //both the pointers were validated but the caller has for some reason requested to read 0 bytes
    
    struct ktfs_file * file = (void *)uio - offsetof(struct ktfs_file , base);
    int retval;
    int firstlen = len;
    int secondlen = 0;
    
    
    trace("file->pos: %d\n", file->pos);
    if (file->pos + len > file->inode_data.size){
        firstlen = file->inode_data.size - file->pos;
        secondlen = len - firstlen;
    } 
    trace("firstlen: %d\n", firstlen);
    trace("secondlen: %d\n", secondlen);
    

    if (!file->opened) return -EINVAL;

    unsigned long nstored = 0;
    unsigned long nwritten;
    uint32_t absolute_idx;
    struct ktfs_data_block *cache_block;

    //case one: overwriting the file
    while (nstored < firstlen){
        //if (firstlen == 0) trace("null entry");
        nwritten = MIN(KTFS_BLKSZ - file->pos%KTFS_BLKSZ, firstlen - nstored); //chooses between the didtance btween the pos and the next block, or whatevers left to fetch
        
        absolute_idx = ktfs_get_block_absolute_idx(ktfs->cache_ptr, &file->inode_data, file->pos/KTFS_BLKSZ);
        if (absolute_idx < 0) {
            trace("absolute_idx <0");
            return absolute_idx; //propagate error
        }

        trace("a number that is pretty important to us at this point: %d", absolute_idx*KTFS_BLKSZ);
        retval = cache_get_block(ktfs->cache_ptr, absolute_idx*KTFS_BLKSZ, (void *)&cache_block);
        if (retval < 0){
            trace("cache_get_block failed: retval: %d\n", retval);
            cache_release_block(ktfs->cache_ptr, cache_block, 0);
            return retval;
        }
        trace("ok so at this point we should have cache_get_blocked something lets see what happened about it");
        memcpy((char *)buf+nstored, (char *)(cache_block->data)+(file->pos % KTFS_BLKSZ),nwritten); //TODO... THIS WHOLE THING WAS PASTED FROM FETCH
        cache_release_block(ktfs->cache_ptr, cache_block, 1);

        nstored += nwritten;
        file->pos += nwritten;
    }

    //second case: append to end

    nstored += ktfs_appender(ktfs->cache_ptr, &file->inode_data, (char*)buf+nstored, secondlen, F_APPEND_STORE); //keep in mind that this will return 0 if second len is 0
    trace("possibly another append issue?");
    return nstored; 
}

/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */
int ktfs_create(struct filesystem* fs, const char* name) { 
    trace("%s(fs: %p, name :%s)\n",__func__, fs, name);
    //trace("ktfs->fs: %p\n", &ktfs->fs);
    //trace("fs: %p\n", fs);
    struct ktfs * ktfs_inst = (void *)fs - offsetof(struct ktfs, fs); //just incase we move towards multiple mountable ktfs
    //if (ktfs_inst->cache_ptr != ktfs->cache_ptr) trace("ktfs_inst->cache_ptr: %p != ktfs->cache_ptr: %p\n", ktfs_inst->cache_ptr, ktfs->cache_ptr);
    
    void * blkptr;
    
    if (!fs || !name) return -EINVAL;
    if (strlen(name) > KTFS_MAX_FILENAME_LEN) return -ENOTSUP;
    //trace("ktfs->max_inode_count in ktfs create: %d\n", ktfs->max_inode_count);
    //trace("ktfs_inst->root_directory_inode_data.size: %d and ktfs_inst->max_inode_count: %d\n", ktfs_inst->root_directory_inode_data.size/KTFS_DENSZ, ktfs_inst->max_inode_count);
    if (ktfs_inst->root_directory_inode_data.size/KTFS_DENSZ >= ktfs_inst->max_inode_count){
        trace("the rdr was jacked up at this point\n");
        return -EINVAL;//this is probably the most robust way of doing this without reading the rdr 
    }
    trace("ktfs_create got: got through first 3 guard cases\n");

    int i = 0; //search for file with matching name
    while (i< ktfs_inst->max_inode_count){
        if (records->filetab[i] && !strncmp(name, records->filetab[i]->dentry.name, KTFS_MAX_FILENAME_LEN)) return -EEXIST;
        i++;
    }
    //if (i != ktfs_inst->max_inode_count) return -EEXIST; //file found. cannot create a new one with the same name
    
    //trace("reached\n"); WHY THE HELL DOES THIS CAUSE A LOAD ACCESS FAULT
    
    //first see if we can even get another inode slot
    struct ktfs_dir_entry dentry;    
    dentry.inode = ktfs_find_and_use_free_inode_slot(ktfs_inst->cache_ptr);

    if (dentry.inode< 0) {
        trace("our find free inode funciton might be the one having issues\n");
        return dentry.inode; 
    }
    strncpy(dentry.name, name, KTFS_MAX_FILENAME_LEN);
    
    //actually write the inode into the file system (u forgot this u dingus):
    //cache_get_block(cache, (dentry.inode wait yuo don't actually have to do that u dingus)
    //wait TODO: you MUST memset it to 0 because we don't want the inode to have data rigth????

    //get and preupdate root directory inode 
    //struct ktfs_inode rdr_copy; //= kcalloc(1, sizeof(struct ktfs_inode)); //to my past self : stfu I'm doing it on the stack //to my past self who was talking to my past self : wtf ru talking about we literally have a bookeeping member for this in the ktfs struct
    struct ktfs_inode * rdr_copy = &ktfs_inst->root_directory_inode_data;
    cache_get_block(ktfs_inst->cache_ptr, ((ktfs_inst->root_directory_inode/KTFS_NUM_INODES_IN_BLOCK)+ ktfs_inst->inode_block_start)*KTFS_BLKSZ, &blkptr);
    struct ktfs_inode * rdr = (struct ktfs_inode*)blkptr + (ktfs_inst->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK);
    //trace("ktfs->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK: %d\n",ktfs_inst->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK); fixed
    trace("line %d: rdr_copy->size: %d\n",__LINE__, rdr_copy->size);
    memcpy(rdr_copy, rdr, KTFS_INOSZ); 
    if (rdr->size + KTFS_DENSZ > KTFS_MAX_FILE_SIZE){
        trace("rdr->size: %d, which is too big\n", rdr->size);
        cache_release_block(ktfs_inst->cache_ptr, blkptr, 0);
        return -ENOTSUP;
    }
    //rdr->size += KTFS_DENSZ; yo the rdr is actually designed to be updates in the helper function so this was completely unnessesary
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 1);
    

    //append file onto root directory inode
    ktfs_appender(ktfs_inst->cache_ptr, rdr_copy, (void *)&dentry, KTFS_DENSZ, F_APPEND_CREATE);
    
    cache_get_block(ktfs_inst->cache_ptr, ((ktfs->root_directory_inode/KTFS_NUM_INODES_IN_BLOCK)+ ktfs->inode_block_start)*KTFS_BLKSZ, &blkptr);//found the mistake... how did you do that twice???
    rdr = (struct ktfs_inode*)blkptr + (ktfs->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK);
    trace("rdr_copy->size: %d and rdr->size: %d\n", rdr_copy->size, rdr->size);
    assert(rdr_copy->size == rdr->size);//well yeah after I do the append these should be the same or we have a huge problem : "ok so we have a huge problem" - present self
    memcpy((void*)rdr, (void*)rdr_copy, KTFS_INOSZ);
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 1); 



    //memset inode TODO: DO WE NEED TO KEEP THIS?
    cache_get_block(ktfs_inst->cache_ptr, (dentry.inode/KTFS_NUM_INODES_IN_BLOCK+ktfs_inst->inode_block_start)*KTFS_BLKSZ, &blkptr);//error here, should've been inode blockstart
    memset((struct ktfs_inode *)blkptr+ dentry.inode%KTFS_NUM_INODES_IN_BLOCK, 0, KTFS_INOSZ);
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 1);

    //add new ktfs_file to filetab
    struct ktfs_file * new_file = kcalloc(1, sizeof(struct ktfs_file));
    for (int i = 1; i < ktfs_inst->max_inode_count; i++){ //already checked that we're not exceeding the max number of files that our filesystem image can support
        if (records->filetab[i] == NULL) {
            records->filetab[i] = new_file; 
            trace("put on index i: %d\n", i);
            memcpy(&records->filetab[i]->dentry, &dentry, KTFS_DENSZ);//only thing we really need to replace otherwise remember memset makes everyhting 0. which is what we want

            struct uio_intf * file_uio_intf = kcalloc(1, sizeof(struct uio_intf)); //nope never mind we want this too... (copied from mount_ktfs)
            memcpy(file_uio_intf, &initial_file_uio_intf, sizeof(struct uio_intf));
            uio_init0(&records->filetab[i]->base, file_uio_intf);
            return 0;
        }
    }

    trace("since we've already checked whether or not we have the facilities to support a new file, it would be really weird if we reached this point\n");
    return -EINVAL; 


}

/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */
int ktfs_delete(struct filesystem* fs, const char* name) {

    trace("%s(fs: %p, name :%s)\n",__func__, fs, name);
    struct ktfs * ktfs_inst = (void *)fs - offsetof(struct ktfs, fs); //just incase we move towards multiple mountable ktfs
    void * blkptr;



    if (!fs || !name) return -EINVAL;
    if (strlen(name) > KTFS_MAX_FILENAME_LEN) return -ENOTSUP;
    if (ktfs_inst->root_directory_inode_data.size/KTFS_DENSZ < 1) return -EINVAL;

    //search for file with matching name
    int i = 0;
    while (i< ktfs_inst->max_inode_count){
        // used to be || and that caused an error for some reason this fixed it but I'm too tired to make sure... my traces are all over the place and I probably read the wrong one which sent me down an insane rabbithole
        if (records->filetab[i] && !strncmp(name, records->filetab[i]->dentry.name, KTFS_MAX_FILENAME_LEN)) break;
        i++;
    }
    if (i == ktfs_inst->max_inode_count) return -ENOENT; //file not found
    if (records->filetab[i]->opened) return -EBUSY;//TODO: is this a good guardcase???????

    //decrement count in the root_directory Inode: 
    //copy rdr onto bookkeeping (good measure), 
    //decrement on bookkeeper and disk, and then release the block (if size goes down indices don't have to be updated) TODO: MAKE ABSOLUTELY SURE!!!!!
    struct ktfs_inode * rdr_copy = &ktfs_inst->root_directory_inode_data;
    cache_get_block(ktfs_inst->cache_ptr, (ktfs_inst->root_directory_inode/KTFS_NUM_INODES_IN_BLOCK+ ktfs_inst->inode_block_start)*KTFS_BLKSZ, &blkptr);
    struct ktfs_inode* rdr = (struct ktfs_inode*)blkptr + (ktfs->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK);
    trace("the inode number that we're looking at: %d\n",ktfs->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK);
    if (rdr->size/KTFS_INOSZ < 1){
        cache_release_block(ktfs_inst->cache_ptr, blkptr, 0);
        return -7059;//ok for some reason something changed the root directory inode in the bookkeeping structure without us knowing?????
    }

    trace("line %d: rdr_copy size:%d\n",__LINE__, ktfs_inst->root_directory_inode_data.size);
    rdr->size -= KTFS_DENSZ;                                                                                         //note that we want to find the LAST datablock, not the start of the next one, which could happen if we have a full block at the end. for this case just decrement the size of a dentry before calling the function
    int last_db_abs_idx = ktfs_get_block_absolute_idx(ktfs_inst->cache_ptr, rdr, rdr->size/KTFS_NUM_DENTRY_IN_BLOCK);//before we decrement, find the absolute index of the last data-block in the inode

    trace("line %d: rdr_copy size:%d\n",__LINE__, ktfs_inst->root_directory_inode_data.size);
    memcpy(rdr_copy, rdr, KTFS_INOSZ);
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 1);


    trace("line %d: rdr_copy size:%d\n",__LINE__, ktfs_inst->root_directory_inode_data.size);
    //remove file from file records
    kfree(records->filetab[i]);
    records->filetab[i] = NULL;
    //TODO: should we memset the inode on the disk too?????? could help with debugging but it shouldn't matter because theres no dentry pointing to it

    //remove dentries 
    cache_get_block(ktfs_inst->cache_ptr, last_db_abs_idx*KTFS_BLKSZ, &blkptr); 
    struct ktfs_dir_entry * dentry = (struct ktfs_dir_entry*)blkptr + (rdr_copy->size%KTFS_BLKSZ)/KTFS_DENSZ;//the way this is decremented works out super nicely. kinda i^c selected it lol
    //TODO: again should I be memsetting? I'm starting to think the answer is no
    //TODO:TODO:TODO: MAKE SURE THAT THE INODE GETS DEALLOCATED - best to do here since we're going to have to find the inode we want to replace and actually replace it. we can read the data here and make the replacement is what I mean to say
    ktfs_free_inode_slot(ktfs_inst->cache_ptr, dentry->inode);
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 1);

    trace("line %d: rdr_copy size:%d\n",__LINE__, ktfs_inst->root_directory_inode_data.size);
    if (rdr_copy->size%KTFS_BLKSZ != 0) return 0; //ez dubz
    
    //not so easy dubz: 
    //TODO:TODO:      deallocate the last datablock immediately
    ktfs_free_db_slot(ktfs_inst->cache_ptr, last_db_abs_idx);

    //TODO consider memsetting ... again
    
    //
    uint32_t size = rdr->size;
    if (size/KTFS_BLKSZ <KTFS_NUM_DIRECT_DATA_BLOCKS){
        return 0;//the leaf comes straight from the pointer in this case -> we;re already done because we/ve already deallocated the leaf
    }
    size-= KTFS_NUM_DIRECT_DATA_BLOCKS;
    if (size/KTFS_BLKSZ < 128){
        //we've deallocated the leaf... all thats left for this case is the first level if it needs to be deleted
        if (size == 0) ktfs_free_db_slot(ktfs_inst->cache_ptr, last_db_abs_idx - ktfs_inst->data_block_start);
        return 0;

    }
    size-= 128;
    if (size/KTFS_BLKSZ < 2*128*128){
        return -ENOTSUP;//MASSIVE TODO TODO TODO TODO: finish implementing dindirect deallocation here
    }

    return -EINVAL;//just like with create, something went wrong if you ended up here (how did oyu manage to have an rdr size bigger than the max_filesize)
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

    struct ktfs_file * file = (void *)uio - offsetof(struct ktfs_file ,base);
    //uint32_t size = file->inode_data.size;
    //trace("%s : size: %d\n", __func__, size);

    if (!file->opened) return -EINVAL;

    switch (cmd)
    {
    case FCNTL_GETEND:

        *((uint32_t*)arg) = file->inode_data.size; // FCNTL_GETEND should pass back the size of the file in bytes through the arg variable
        return 0;

        break;

    case FCNTL_SETEND:
        
        //uint32_t end = *((uint32_t*)arg);
        uint32_t end = *((uint32_t*)arg);
        if (end < file->inode_data.size) return -ENOTSUP; //we aren't supposed to support shortening files I'm pretty sure
        if (end == file->inode_data.size) return 0;

        int retval =  ktfs_appender(ktfs->cache_ptr, &file->inode_data, NULL, end - file->inode_data.size, F_APPEND_SETEND);
        if (retval <0) return retval;
        else return 0;
        break;

    case FCNTL_GETPOS:

        *((uint32_t*)arg) = file->pos; // FCNTL_GETPOS should pass back the current position of the file pointer in bytes through * the arg variable
        return 0;

        break;

    case FCNTL_SETPOS: 

        file->pos = *((uint32_t*)arg); // FCNTL_SETPOS should set the current position of the file pointer to the value passed in * through arg
        return 0 ;

        break;

    default:
        break;
    }

    return -EINVAL ; // otherwise, negative error code

}

/**
 * @brief Flushes the cache to the backing device
 * @return 0 if flush successful, negative error code if error
 */
void ktfs_flush(struct filesystem* fs) {
    // FIXME

    // EDGE CASE : Check if cache exists
    if(ktfs->cache_ptr == NULL) return;// -EINVAL ;

    // Return val for success (0) and negative error code handled in cache_flush NOT NEEDED
    //int retval = 
    cache_flush(ktfs->cache_ptr) ;
    
    
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
