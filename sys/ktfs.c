/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‌‌​‌‍‍⁠​⁠⁠‌
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/  
/* mkfs command
*/


// high priority todo list
/*
 *
 * ask TA about negative return when no file is found on ktfs_open
 * figure out passing in NULL double pointers. If it isn't ok then change it everywhere and adjust guardcases
*/

// low priority todo list
/*
 * multiple mountpoints
 * ask TA about negative return when no file is found on ktfs_open
 * legacy issue. figure out passing in NULL double pointers. If it isn't ok then change it everywhere and adjust guardcases
*/


//#include <cstdint>
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
	uint32_t dentry_slot;
    struct ktfs_inode inode_data; // we fill out the inode data when we open the file. when we close the file, free it and set the pointer to null. 
};

struct ktfs_file_records{
    uint64_t reserved;
    struct ktfs_file * filetab[];
};


struct ktfs_listing_uio {
    struct uio base;
    int read_idx;
    const struct ktfs_file_records * records;
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

static const struct uio_intf ktfs_listing_uio_intf  = {
    .close = &ktfs_listing_close,
    .read = &ktfs_listing_read
};

//the next two are exactly what it sounds like
//NOTE: db_blk_num/inoed_slot_num is the DATA_BLOCK_INDEX/INODE_SLOT_INDEX of the block we're trying to free, not the absolute index like we've so often used for other function
int ktfs_free_db_slot(struct cache* cache, uint32_t db_blk_num){
    void * blkptr;
	int abs_blk_of_bitmap = db_blk_num/(KTFS_BLKSZ*8)+ktfs->bitmap_block_start;
    cache_get_block(cache, abs_blk_of_bitmap*KTFS_BLKSZ, &blkptr);
	int index = (db_blk_num%(KTFS_BLKSZ*8))/8;
	int offset = (db_blk_num%(KTFS_BLKSZ*8))%8;
	if ((((char *)blkptr)[index] & (0x01<<offset)) == 0) trace("warning: decided to free a datablock that has already been freed");//comment out this line when its time to submit
	((char *)blkptr)[index] &= ~(0x01<<offset);
    cache_release_block(cache, blkptr, 1);
    return 0;
}
int ktfs_free_inode_slot(struct cache* cache, uint32_t inode_slot_num){
    void * blkptr;
	int abs_blk_of_bitmap = inode_slot_num/(KTFS_BLKSZ*8)+ktfs->inode_bitmap_block_start;
    cache_get_block(cache, abs_blk_of_bitmap*KTFS_BLKSZ, &blkptr);	
	int index = (inode_slot_num%(KTFS_BLKSZ*8))/8;
	int offset = (inode_slot_num%(KTFS_BLKSZ*8))%8;
	if ((((char *)blkptr)[index] & (0x01<<offset)) == 0) trace("warning: decided to free an inode that has already been freed");//comment out this line when its time to submit
	((char *)blkptr)[index] &= ~(0x01<<offset);
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
    uint16_t inode_num; //specifically the number of the inode (we already have the root directory inode number as well)

    //guard cases
    if (bytecnt ==0) return 0;
    if (op != F_APPEND_SETEND && op != F_APPEND_STORE && op != F_APPEND_CREATE) return -ENOTSUP;


    if (op == F_APPEND_SETEND || op == F_APPEND_STORE){
        file_wrapper = (void *)inode - offsetof(struct ktfs_file, inode_data);
        if (file_wrapper->pos != inode->size) return -ENOTSUP; //Dawg I just told you, this function can only be called if we're at the end of the file
        inode_num = file_wrapper->dentry.inode;
    }
    else if (op==F_APPEND_CREATE){
        //inode = &ktfs->root_directory_inode_data; //no filewrapper HOWEVER. WE CAN JUST PASS IN THE RDR COPY THAT WE KEEP INSIDE OF THE KTFS STRUCT
		assert(inode = &ktfs->root_directory_inode_data); //as a matter of fact
//        trace("size in appender: %d\n",ktfs->root_directory_inode_data.size);
        inode_num = ktfs->root_directory_inode;
    }


    //ensures that we don't somehow exceed the maximum filesize
    bytecnt = MIN(bytecnt, KTFS_MAX_FILE_SIZE - inode->size);
	
	trace("inode num: %d\n", inode_num);
	if (op!=F_APPEND_CREATE) trace("file name: %s\n", file_wrapper->dentry.name);

    int nstored = 0;
    void * blkptr;
    while (nstored < bytecnt){
        int allocated_index;
        if (inode->size % KTFS_BLKSZ == 0) {
            trace("allocated the datablock: %d\n", inode->size/KTFS_BLKSZ);
            allocated_index = ktfs_alloc_datablock(cache, inode, inode->size/KTFS_BLKSZ); //in ANY case where pos is exactly a multiple of blksz, that means we're at the beginning of a new block
            if (allocated_index <0){
                trace("alloc_datablock returned a negative value: %s\n", error_name(allocated_index));
                return allocated_index;
            }//should always allocate in this case
			//trace("allocated_index: %d\n", allocated_index);
        }
        else allocated_index = ktfs_get_block_absolute_idx(cache, inode, inode->size/KTFS_BLKSZ); 

        int n_per_cycle = MIN(KTFS_BLKSZ - inode->size%KTFS_BLKSZ, bytecnt - nstored);
        //actual store part
        if (cache_get_block(cache, allocated_index*KTFS_BLKSZ, &blkptr)<0){// (can make this a noop for setend and operations on the root directory inode, although that's a pretty trivial optimization)
            trace("cache_get_block returned a negative value\n");
            return -EINVAL;
        }

        if (op == F_APPEND_SETEND) memset(blkptr+inode->size%KTFS_BLKSZ, 0, n_per_cycle);
        else memcpy(blkptr+inode->size%KTFS_BLKSZ, buf+nstored, n_per_cycle);

        cache_release_block(cache, blkptr, 1); //we wrote to a block so its dirty
        
        //increment values
        nstored += n_per_cycle;
        //*pos += n_per_cycle;//do this at the end
		inode->size +=n_per_cycle;
        //if (op != F_APPEND_CREATE) inode->size += n_per_cycle; //because for "create" there is no filewrapper, and the pos itsself points to size

    }
	if (op != F_APPEND_CREATE) file_wrapper->pos = inode->size;

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

	kprintf("size before memcpy: %d\n",((struct ktfs_inode *)blkptr)[inode_num].size);
    memcpy((char *)blkptr +(inode_num%KTFS_NUM_INODES_IN_BLOCK)*KTFS_INOSZ, inode, KTFS_INOSZ);//memcpy the inode back into the inode blocks at the correct position (given by inode_num% KTFS_NUM_INODES_IN_BLOCK
	kprintf("size after memcpy: %d\n",((struct ktfs_inode *)blkptr)[inode_num].size);
    cache_release_block(cache, blkptr, 1);
	
    trace("nstored: %d\n", nstored);
    return nstored;
}


/*
* @ brief: 
* @ parameters: 
* @ return: (for convience) the absolute index of the 
*/
int ktfs_alloc_datablock(struct cache* cache, struct ktfs_inode * inode, uint32_t contiguous_db_to_alloc){
    trace("%s(cache=%p, inode=%p, contiguous_db_to_alloc=%u)\n", __func__, cache, inode, contiguous_db_to_alloc);
    int alloc_db_idx;
    void * blkptr;

    //one by one checks needs for direct, indirect, and dindirect allocation
    if (contiguous_db_to_alloc < KTFS_NUM_DIRECT_DATA_BLOCKS){
        alloc_db_idx = ktfs_find_and_use_free_db_slot(cache);
        trace("alloc_db_idx: %d\n" , alloc_db_idx);
        if (alloc_db_idx <0) return alloc_db_idx;
        inode->block[contiguous_db_to_alloc] = alloc_db_idx;  
        return alloc_db_idx + ktfs->data_block_start;
    }
    contiguous_db_to_alloc -= KTFS_NUM_DIRECT_DATA_BLOCKS;


    if (contiguous_db_to_alloc< 128){
    
	//kprintf("reached indirect blocks in %s\n", __func__);

        if (contiguous_db_to_alloc == 0){ //case that its the first block we need to store in the indirects, so there isn't already a datablock indirection
            alloc_db_idx = ktfs_find_and_use_free_db_slot(cache);
			trace("alloc_db_idx: %d\n", alloc_db_idx);
            if (alloc_db_idx < 0){
				kprintf("ktfs_find_and_use_free_db_slot returned error: %s\n", error_name(alloc_db_idx));
				return alloc_db_idx;
			}
            inode->indirect = alloc_db_idx;
        }
        
        int new_indir_blk = ktfs_find_and_use_free_db_slot(cache);//allocate new indirect blk (leaf)
		
		kprintf("new_indir_blk %d\n", new_indir_blk);
        if (new_indir_blk < 0){
            //inode->indirect = 0;//don't see why to do that
            return new_indir_blk;
        }
		
        //write new pointer into the indirect blk
        if (cache_get_block(cache, (inode->indirect + ktfs->data_block_start) * KTFS_BLKSZ, &blkptr)< 0){
            kprintf("cache_get_block returned a negative value\n");
            return -EINVAL;
        }
        ((uint32_t*)blkptr)[contiguous_db_to_alloc] = (uint32_t)new_indir_blk;
        cache_release_block(cache, blkptr, 1);
		
        return new_indir_blk + ktfs->data_block_start; //REMEBER THIS FUNCTION ALWAYS RETURNS THE LEAF OF THE SEQUENCE THATS ALLOCATED	
        
    }

    contiguous_db_to_alloc -= 128;
    
    //TODO: dindirect case
    //
    if (contiguous_db_to_alloc >= 2*128*128) return -ENOTSUP;

	//kprintf("reached dindirect blocks in %s\n", __func__);

    int lvl_two_alloc_db = -1;
    int lvl_one_alloc_db = -1;

    if (contiguous_db_to_alloc % 128 == 0){ //means we have to AT LEAST do the second level of allocation
        if (contiguous_db_to_alloc % (128*128) ==0){
            lvl_one_alloc_db = ktfs_find_and_use_free_db_slot(cache);
            if (lvl_one_alloc_db < 0){
                trace("error from find and use free db");
                return lvl_one_alloc_db;
            }
            inode->dindirect[contiguous_db_to_alloc/(128*128)] = lvl_one_alloc_db;
        }else lvl_one_alloc_db = inode->dindirect[contiguous_db_to_alloc/(128*128)]; //= lvl_two_alloc_db;
        
        lvl_two_alloc_db = ktfs_find_and_use_free_db_slot(cache);
        cache_get_block(cache, (ktfs->data_block_start+ lvl_one_alloc_db)*KTFS_BLKSZ, &blkptr);
        ((uint32_t *)blkptr)[(contiguous_db_to_alloc%(128*128))/128] = lvl_one_alloc_db;
        cache_release_block(cache, blkptr, 1);
    }
    else{
        lvl_one_alloc_db = inode->dindirect[contiguous_db_to_alloc/(128*128)];
        cache_get_block(cache, (ktfs->data_block_start + lvl_one_alloc_db)*KTFS_BLKSZ, &blkptr);
        lvl_two_alloc_db = ((uint32_t *)blkptr)[(contiguous_db_to_alloc%(128*128))/128];
        cache_release_block(cache, blkptr, 0);
    }

    //at this point we will always have a lvl2 block

    int new_leaf_db = ktfs_find_and_use_free_db_slot(cache);

    cache_get_block(cache, (lvl_two_alloc_db+ ktfs->data_block_start)*KTFS_BLKSZ, &blkptr);
    ((uint32_t *)blkptr)[contiguous_db_to_alloc%128] = new_leaf_db;
    cache_release_block(cache, blkptr, 1);
    return new_leaf_db; 
}


//finds AND CLAIMS the first free datablock on the bitmap. also marks the slot as used before returning
//simply a helper function for ktfs_alloc_datablock, since I'd have to write the smae code over and over again for otherwise
//NOTE: RETURNS THE DATABLOCK IDX NOT ABSOLUTE IDX
//note that if this function returned a negative, the allocation should have ABSOLUTELY NEVER HAPPENED
int ktfs_find_and_use_free_db_slot(struct cache * cache){

    void * blkptr;


    int n_db = (ktfs->block_cnt - ktfs->data_block_start);
    int curr_db = 0;

    while(curr_db < n_db){

        int ndb_per_cycle = MIN(KTFS_BLKSZ*8, n_db - curr_db);

        if (cache_get_block(cache, (ktfs->bitmap_block_start + curr_db/(KTFS_BLKSZ*8))*KTFS_BLKSZ, &blkptr)<0) {trace("wtf"); return -900;}
        
        for (int i = 0; i < ndb_per_cycle; i++){
            int cycle_offset = i / 8;
            int loop_offset = i % 8;
            if((((char *)blkptr)[cycle_offset] & (1<< loop_offset))== 0){
                if (i==20) trace("there is no way we can be here\n");
                ((char *)blkptr)[cycle_offset] |= (1<<loop_offset);
                cache_release_block(cache, blkptr, 1);
				
    			trace("%s, claims dbs: %d\n", __func__, curr_db);
                return curr_db;
            }
			curr_db++;
        }
        cache_release_block(cache, blkptr, 0);

            //curr_db+= ndb_per_cycle;
    }

    return -ENODATABLKS;
}


//finds AND CLAIMS the first free datablock on the bitmap. also marks the slot as used before returning
//simply a helper function for ktfs_alloc_datablock, since I'd have to write the smae code over and over again for otherwise
//NOTE: RETURNS THE DATABLOCK IDX NOT ABSOLUTE IDX
int ktfs_find_and_use_free_inode_slot(struct cache * cache){
	trace("ktfs_find_and_use_free_inode_slots\n");
    void * blkptr;

    cache_get_block(ktfs->cache_ptr, ktfs->inode_bitmap_block_start*KTFS_BLKSZ, &blkptr);

    //NOTE HARDCODED TO 512 instead of using ktfs_max_inode_num;
    for (int i = 0; i < 512; i++){
        int index = i / 8;
        int offset = i % 8;
        if((((char * )blkptr)[index] & (1<< offset))== 0){
            ((char *)blkptr)[index] |= (1<<offset);
            cache_release_block(ktfs->cache_ptr, blkptr, 1);   
            return i;
        }

    }
    cache_release_block(ktfs->cache_ptr, blkptr, 0);
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

	trace("welcome to dindirects\n");
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
    //trace("pre kcalloc\n");
    //ktfs->fs = kcalloc(1, sizeof(struct filesystem)); //post cp1 change: fs member is no longer pointer 
    //trace("post kcalloc\n");
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
		records->filetab[i]->dentry_slot = i;//cp2 addition
 
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
    
    //NOTE: CP3 ADDITION: LISTING!!!!!!!!!
    if (!strncmp(name, "" , strlen(name)) || !strncmp(name, "/", strlen(name))){
        struct ktfs_listing_uio * ls;
        ls = kcalloc(1, sizeof(*ls));
        ls->read_idx = 0;
        ls->records = records;
        *uioptr  = uio_init1(&ls->base, &ktfs_listing_uio_intf);
        return 0;
    }

    //"search for inode that matches name" section //
    int i = 0;
    while (i < ktfs->max_inode_count){
        if (records->filetab[i]) trace("name: %s, records->filetab[i]->dentry.name: %s\n",name, records->filetab[i]->dentry.name);
        if (records->filetab[i] && !strncmp(name, records->filetab[i]->dentry.name, KTFS_MAX_FILENAME_LEN)){
            
            break;
        }

        i++;
    }
    
    //guardcase for no matching file found 
    trace("i: %d\n",i);
    trace("max_inode_count = %d\n", ktfs->max_inode_count);
    if (i == ktfs->max_inode_count){
		trace("no matching file found\n");
		return -ENOENT; 
	}

    //guardcase for already opened 
    if (records->filetab[i]->opened ==1) return -EBUSY;

    //this wasn't nessesary because I already have an opened flag which is 1 whenever there is atleast one uioptr referencing it
    //guardcase for if the file is already referenced by another uioptr
    //if (uio_refcnt(&records->filetab[i]->base) >0) return -EBUSY; //it decrements to 0 anyway on close
    //else uio_addref() happens towards the end

	//could maybe have a dentry slot assertion here

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
    int printflag = 0;
	trace("file->pos: %d\n", file->pos);
	//trace("string: %s",buf);
    //case one: overwriting the file
    while (nstored < firstlen){
        //if (firstlen == 0) trace("null entry\n");
        nwritten = MIN(KTFS_BLKSZ - file->pos%KTFS_BLKSZ, firstlen - nstored); //chooses between the didtance btween the pos and the next block, or whatevers left to fetch
        
        if (printflag == 0){ 
            kprintf("overwrite case reached\n");
            printflag++;
        }
        absolute_idx = ktfs_get_block_absolute_idx(ktfs->cache_ptr, &file->inode_data, file->pos/KTFS_BLKSZ);
        if (absolute_idx < 0) {
            kprintf("absolute_idx <0");
            return absolute_idx; //propagate error
        }

        //trace("a number that is pretty important to us at this point: %d", absolute_idx*KTFS_BLKSZ);
        retval = cache_get_block(ktfs->cache_ptr, absolute_idx*KTFS_BLKSZ, (void *)&cache_block);
        if (retval < 0){
            kprintf("cache_get_block failed: retval: %d\n", retval);
            cache_release_block(ktfs->cache_ptr, cache_block, 0);
            return retval;
        }
        trace("ok so at this point we should have cache_get_blocked something lets see what happened about it");
        //memcpy((char *)buf+nstored, (char *)(cache_block->data)+(file->pos % KTFS_BLKSZ),nwritten); //TODO... THIS WHOLE THING WAS PASTED FROM FETCH
        memcpy((char *)(cache_block->data)+(file->pos % KTFS_BLKSZ), (char *)buf+nstored,nwritten); //TODO... THIS WHOLE THING WAS PASTED FROM FETCH
        cache_release_block(ktfs->cache_ptr, cache_block, 1);

        nstored += nwritten;
        file->pos += nwritten;
    }

    //second case: append to end
    if (secondlen != 0) kprintf("append case reached\n");

    nstored += ktfs_appender(ktfs->cache_ptr, &file->inode_data, (char*)buf+nstored, secondlen, F_APPEND_STORE); //keep in mind that this will return 0 if second len is 0
	trace("file data after store:\n");
	trace("file size: %d\n", file->inode_data.size);

    //trace("possibly another append issue?");
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
    if (strlen(name) > KTFS_MAX_FILENAME_LEN) {
		trace("file name is too big");
		return -ENOTSUP;
	}
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
    
    //first see if we can even get another inode slot
    struct ktfs_dir_entry dentry; //if we have size for one, this will be what we memset onto the filesystem book
    dentry.inode = ktfs_find_and_use_free_inode_slot(ktfs_inst->cache_ptr);

	trace("inoed of choice: dentry.inode= %d\n", dentry.inode);
    if (dentry.inode< 0) {
        trace("our find free inode funciton might be the one having issues\n");
        return dentry.inode; 
    }

    strncpy(dentry.name, name, KTFS_MAX_FILENAME_LEN);
    

    //get and preupdate root directory inode 
    struct ktfs_inode * rdr_copy = &ktfs_inst->root_directory_inode_data;
	trace("calculated rdr block to be at: %d\n", ((ktfs_inst->root_directory_inode/KTFS_NUM_INODES_IN_BLOCK)+ ktfs_inst->inode_block_start));
    cache_get_block(ktfs_inst->cache_ptr, ((ktfs_inst->root_directory_inode/KTFS_NUM_INODES_IN_BLOCK)+ ktfs_inst->inode_block_start)*KTFS_BLKSZ, &blkptr);
    struct ktfs_inode * rdr = (struct ktfs_inode*)blkptr + (ktfs_inst->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK);
    memcpy(rdr_copy, rdr, KTFS_INOSZ);
    if (rdr->size + KTFS_DENSZ > KTFS_MAX_FILE_SIZE){
        trace("rdr->size: %d, which is too big\n", rdr->size);
        cache_release_block(ktfs_inst->cache_ptr, blkptr, 0);
        return -ENOTSUP;
    }	
    int new_dentry_slot = rdr->size/KTFS_DENSZ;//cp2 addition -1 because we are doing the lil "bulk allocation" thing they taguht us in mp1
    rdr->size += KTFS_DENSZ; //preincrement the size of the root directory inode
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 1);
    

	trace("reached\n");
    //append file onto root directory inode
    ktfs_appender(ktfs_inst->cache_ptr, rdr_copy, (void *)&dentry, KTFS_DENSZ, F_APPEND_CREATE);
	trace("size of rdr on new file, and before redundant memcpy: %d\n",ktfs_inst->root_directory_inode_data.size);
    cache_get_block(ktfs_inst->cache_ptr, 
					((ktfs->root_directory_inode/KTFS_NUM_INODES_IN_BLOCK)+ ktfs->inode_block_start)*KTFS_BLKSZ, 
					&blkptr);
    rdr = (struct ktfs_inode*)blkptr + (ktfs->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK); //I think the appender might already work on this logic... never mind what about new datablocks? 
																								//could look into adding this logic for CREATE case in the appender.
    trace("rdr_copy->size: %d and rdr->size: %d\n", rdr_copy->size, rdr->size);
    assert(rdr_copy->size == rdr->size);
    memcpy((void*)rdr, (void*)rdr_copy, KTFS_INOSZ);
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 1);
	

    
    //memset inode TODO: DO WE NEED TO KEEP THIS? answer, probably not considering the fact that the parts we don't use are ignored due to file size bookkeeping
    //cache_get_block(ktfs_inst->cache_ptr, (dentry.inode/KTFS_NUM_INODES_IN_BLOCK+ktfs_inst->inode_block_start)*KTFS_BLKSZ, &blkptr);//error here, should've been inode blockstart
    //memset((struct ktfs_inode *)blkptr+ dentry.inode%KTFS_NUM_INODES_IN_BLOCK, 0, KTFS_INOSZ);
    //cache_release_block(ktfs_inst->cache_ptr, blkptr, 1);

    //add new ktfs_file to filetab
    struct ktfs_file * new_file = kcalloc(1, sizeof(struct ktfs_file));
    for (int i = 0; i < ktfs_inst->max_inode_count; i++){ //already checked that we're not exceeding the max number of files that our filesystem image can support
        if (records->filetab[i] == NULL) {
            records->filetab[i] = new_file; 
            trace("put on index i: %d\n", i);
            memcpy(&records->filetab[i]->dentry, &dentry, KTFS_DENSZ);//only thing we really need to replace otherwise remember memset makes everyhting 0. which is what we want
            trace("dentry_slot: %d\n",records->filetab[i]->dentry_slot);
			records->filetab[i]->dentry_slot = new_dentry_slot; //new addition
			kprintf("kid named inode: %s at record index %d\n",records->filetab[i]->dentry.name, i);
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

    if (!fs || !name) return -EINVAL;
    struct ktfs * ktfs_inst = (void *)fs - offsetof(struct ktfs, fs); //just incase we move towards multiple mountable ktfs
    void * blkptr;




    if (strlen(name) > KTFS_MAX_FILENAME_LEN) return -ENOTSUP; //10/22 I'm sure that this is not our delete bug
    if (ktfs_inst->root_directory_inode_data.size/KTFS_DENSZ < 1){
		trace("too many inodes im ktfsed");
		 return -EINVAL;
	}
	

    //race cond starts pretty much here tbh 
    int rdr_blk = ((ktfs_inst->root_directory_inode/KTFS_NUM_INODES_IN_BLOCK)+ ktfs_inst->inode_block_start);
    cache_get_block(ktfs_inst->cache_ptr, rdr_blk * KTFS_BLKSZ, &blkptr); 
    struct ktfs_inode * rdr = (struct ktfs_inode*)blkptr + (ktfs_inst->root_directory_inode%KTFS_NUM_INODES_IN_BLOCK);
    if (rdr->size < KTFS_DENSZ){
        trace("rdr size is less than dentry size, cooked\n");
        cache_release_block(ktfs_inst->cache_ptr, blkptr, 0);
        return -EINVAL;
    }
    rdr->size -= KTFS_DENSZ;
    memcpy(&ktfs_inst->root_directory_inode_data, rdr, KTFS_DENSZ);
    int replacement_dentry = ktfs_inst->root_directory_inode_data.size/KTFS_DENSZ;  
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 1); //this behaviour should be locked down during premptive lowkey

    trace("some delete shenanigans\n");


    int target_filetab_idx = -1;

    int replacer_filetab_idx = -1;
    for (int i = 0; i < ktfs_inst->max_inode_count; i++){
        if (!records->filetab[i]) continue;
        trace("name: %s, dentry_slot: %d\n",records->filetab[i]->dentry.name, records->filetab[i]->dentry_slot);
        if (!strncmp(name, records->filetab[i]->dentry.name, KTFS_MAX_FILENAME_LEN)) target_filetab_idx = i;
        if (records->filetab[i]->dentry_slot  == replacement_dentry){
            trace("records->filetab[i]->dentry_slot: %d replacement_dentry: %d", records->filetab[i]->dentry_slot, replacement_dentry);
            replacer_filetab_idx = i;
        }
    }


    trace("some delete shenanigans: target_filetab_idx: %d\n", target_filetab_idx);
	

    if (target_filetab_idx == -1){
		trace("no file with that name exists\n");
		return -ENOENT; //file not found
	}
    if (records->filetab[target_filetab_idx]->opened) {	
		trace("doing delete on an open file smh\n");
		return -EBUSY;
	}


    //if (!records->filetab[target_filetab_idx]) trace("ur mum"); 
    //if (!records->filetab[replacer_filetab_idx]) trace("ur mum the sequel"); //this is a null

	int target_dentry_slot = records->filetab[target_filetab_idx]->dentry_slot; //initial data
    //int replacement_dentry_slot = records->filetab[replacer_filetab_idx]->dentry_slot;
    int replacement_dentry_slot = replacement_dentry;
    
    struct ktfs_dir_entry replacement_dentry_actual;
    struct ktfs_dir_entry target_dentry_actual; //will be useful later for when we 

    struct ktfs_inode target_inode;

    memcpy(&target_inode,&records->filetab[target_filetab_idx]->inode_data, KTFS_INOSZ);
            trace("line\n");

    if (target_dentry_slot == replacement_dentry_slot) { //no replacement case,
        if (ktfs_inst->root_directory_inode_data.size % KTFS_BLKSZ == 0){
            //note that even when the inode has decremented its size to be right on the boundary, this helper function still picks the next block, which is what we want.
            int abs_blk_to_dealloc = ktfs_get_block_absolute_idx(ktfs_inst->cache_ptr, &ktfs_inst->root_directory_inode_data, ktfs_inst->root_directory_inode_data.size/KTFS_BLKSZ);

            //I feel like copying it here is a good idea since another thread might come and allocate it as soon as free happens
            cache_get_block(ktfs_inst->cache_ptr, abs_blk_to_dealloc*KTFS_BLKSZ, &blkptr);
            memcpy(&target_dentry_actual, blkptr, KTFS_DENSZ);//no real pointer math needed here nicely enough
            cache_release_block(ktfs_inst->cache_ptr, blkptr, 0); //no need to memset it to 0 either thats just a waste of time since its not readable anymore.

            ktfs_free_db_slot(ktfs_inst->cache_ptr, abs_blk_to_dealloc - ktfs_inst->data_block_start); // to my teammates, notice the conversion. both the free for the datablocks and the inodes is relative to the start of their sections
        }
        //free up the "live" bookkeppers tehat we set up in mount so that it doesn't conflict
        records->filetab[target_filetab_idx] = NULL;

        //goto delete_cleanup;//yeah, I don't know why either

        //TODO: helper function that deallocates all blocks taht belong to a ktfs file

        //ktfs_free_inode_slot(ktfs_inst->cache_ptr, target_dentry_actual.inode);//deallocate the inode.  BUT ONLY AFTER WE FREE THE INODE OBJS

    kprintf("target_dentry_slot: %d\n", target_dentry_slot);
    kprintf("replacement_dentry_slot: %d\n", replacement_dentry_slot);
    kprintf("replacer_filetab_idx: %d\n", replacer_filetab_idx);
    kprintf("target_filetab_idx: %d\n", target_filetab_idx);
    kprintf("number of files after second delete case (for myself): %d\n", ktfs_inst->root_directory_inode_data.size/KTFS_DENSZ);

        kprintf("YEAHHH PUNCH IT CHEWIE\n");
        goto delete_cleanup;
        //return 0;
    }

    
    //the replacement case 
    kprintf("reached\n");
    
    //** please note that both of these are in ABSOLUTE indexed*/
    int resident_blk_of_replacement_dentry = ktfs_get_block_absolute_idx(ktfs_inst->cache_ptr, &ktfs_inst->root_directory_inode_data, ktfs_inst->root_directory_inode_data.size/KTFS_NUM_DENTRY_IN_BLOCK);
    int resident_blk_of_target_dentry = ktfs_get_block_absolute_idx(ktfs_inst->cache_ptr, &ktfs_inst->root_directory_inode_data, target_dentry_slot/KTFS_NUM_DENTRY_IN_BLOCK);
    //note avain that the above function cooks even when the size is already decremented. see explanation for instance above.
    //TODO: wait this should actually happen way earlier. because if another thread DOES try to create, it has a ton of time to overlap.... can do it really easily towards the beginning but don't thats a later issue.

    int abs_blk_to_dealloc = ktfs_get_block_absolute_idx(ktfs_inst->cache_ptr, &ktfs_inst->root_directory_inode_data, ktfs_inst->root_directory_inode_data.size/KTFS_NUM_DENTRY_IN_BLOCK);//short replacement here works out regardless
    cache_get_block(ktfs_inst->cache_ptr, resident_blk_of_replacement_dentry*KTFS_BLKSZ, &blkptr);//step one: get a copy of the replacer dentry
    memcpy(&replacement_dentry_actual, (struct ktfs_dir_entry *)blkptr + replacement_dentry_slot%KTFS_NUM_DENTRY_IN_BLOCK, KTFS_DENSZ);
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 0); //there is no case where you have to move this. its instantly readable if you just decrement the size by -KTFS_DENSZ, which we did
    if (ktfs_inst->root_directory_inode_data.size % KTFS_BLKSZ == 0){
        ktfs_free_db_slot(ktfs_inst->cache_ptr, abs_blk_to_dealloc - ktfs_inst->data_block_start);//the dealloc end block case for the non-single remove
    }

    cache_get_block(ktfs_inst->cache_ptr, resident_blk_of_target_dentry*KTFS_BLKSZ, &blkptr);
    memcpy(&target_dentry_actual, (struct ktfs_dir_entry *)blkptr + target_dentry_slot%KTFS_NUM_DENTRY_IN_BLOCK, KTFS_DENSZ);
    memcpy((struct ktfs_dir_entry *)blkptr + target_dentry_slot%KTFS_NUM_DENTRY_IN_BLOCK, &replacement_dentry_actual,KTFS_DENSZ);//swapped out so that now the target dentry has what we need for replacement
    cache_release_block(ktfs_inst->cache_ptr, blkptr, 1);

    trace("line\n");
    //take care of the bookeeping - recall the indicies we got a few: target_filetab_idx, replacer_filetab_idx, and we still have their
    kprintf("target_dentry_slot: %d\n", target_dentry_slot);
    kprintf("replacement_dentry_slot: %d\n", replacement_dentry_slot);
    kprintf("replacer_filetab_idx: %d\n", replacer_filetab_idx);
    kprintf("target_filetab_idx: %d\n", target_filetab_idx);
    kprintf("number of files after second delete case (for myself): %d\n", ktfs_inst->root_directory_inode_data.size/KTFS_DENSZ);

    trace("records->filetab[replacer_filetab_idx]->dentry_slot: %d\n", records->filetab[replacer_filetab_idx]->dentry_slot);

    records->filetab[replacer_filetab_idx]->dentry_slot = target_dentry_slot;
    
    records->filetab[target_filetab_idx] = NULL;


    

delete_cleanup: 

    
    
    ktfs_free_inode_slot(ktfs_inst->cache_ptr, target_dentry_actual.inode);//deallocate the inode. easy money (the inoed was already saved)

    //recall we saved the state of the target dentry. we also have the rdr data saved with the ktfs struct, so we can use that for convenience since we have 2hr left

    int cont =0;

    int tot = target_inode.size/KTFS_BLKSZ;
    int flag_delete_indir_later = 0; //for tail deleteion

    while (cont < tot){
        if (cont >= KTFS_MAX_FILE_SIZE/KTFS_BLKSZ) return -ENOTSUP;

        if (cont < KTFS_NUM_DIRECT_DATA_BLOCKS){
            ktfs_free_db_slot(ktfs_inst->cache_ptr, target_inode.block[cont] - ktfs_inst->data_block_start);
        }
        else if (cont < 132){
            if(cont == KTFS_NUM_DIRECT_DATA_BLOCKS) flag_delete_indir_later = 1; 
            ktfs_free_db_slot(ktfs_inst->cache_ptr, ktfs_get_block_absolute_idx(ktfs_inst->cache_ptr, &target_inode, cont) - ktfs_inst->data_block_start);
        }
        else {
            ktfs_free_db_slot(ktfs_inst->cache_ptr, ktfs_get_block_absolute_idx(ktfs_inst->cache_ptr, &target_inode, cont) - ktfs_inst->data_block_start);

            //if (cont == 132+128*128-1) ktfs_free_db_slot(ktfs_inst->cache_ptr, ktfs_get_block_absolute_idx(ktfs_inst->cache_ptr, &target_inode, cont) - ktfs_inst->data_block_start);
            //if (cont == 132+128*128-1) ktfs_free_db_slot(ktfs_inst->cache_ptr, ktfs_get_block_absolute_idx(ktfs_inst->cache_ptr, &target_inode, cont) - ktfs_inst->data_block_start);

        }
        //else return 0;

        cont++;
    }
    //exit free
    if (flag_delete_indir_later) ktfs_free_db_slot(ktfs_inst->cache_ptr, target_inode.indirect);

    return 0;


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

    if (!file->opened) {
        trace("file wasn't opened yet");
        return -EINVAL;
    }

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
    struct ktfs_listing_uio *const ls = (struct ktfs_listing_uio *)uio;
    kfree(ls);
    //TODO: This implementation is actually quite odd (ripped off of devfs but I have the same questions regardless). who calls uio_close?????
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
    struct ktfs_listing_uio *const ls = (struct ktfs_listing_uio *)uio;

    if (ls->records == NULL) return -EINVAL;

    int nfiles = ktfs->max_inode_count;

    // int ncpy = 0;//number of bytes copied
    // for (int i = 0; i < nfiles; i++){ 
    //     if (!ls->records->filetab[i]) continue;
    //     char * name = ls->records->filetab[i]->dentry.name;
    //     if (ncpy+strlen(name)+1 >= bufsz) break; //TODO: make sure this is right.... I mean the only difference I see in impl is whether i output them as seperate stirngs or one big string of FILE_NAME_MAX_LEN size
    //     memcpy((char *)buf+ncpy, name, strlen(name));
    //     memset((char *)buf+(ncpy+strlen(name)+1), 0, 1);//note: accounted for the null-terminator here
    //     ncpy += strlen(name)+1;
    // }
    

    //new clean implementation
    int ncpy = 0;
    while (ls->read_idx < nfiles){ 
        if (!ls->records->filetab[ls->read_idx]) continue;
        if (bufsz-ncpy <= 0) {
            return ncpy;
        }
        ncpy += snprintf((char *)buf+ncpy, bufsz-ncpy, "%s\r\n", ls->records->filetab[ls->read_idx]);
        ls->read_idx++;
    }
    
    return ncpy;
}

