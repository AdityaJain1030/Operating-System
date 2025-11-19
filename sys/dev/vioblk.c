/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‌‌​‌‍‍⁠​⁠⁠‌
    @brief VirtIO block device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "devimpl.h"
#include <stddef.h>
#include <stdint.h>
#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include <limits.h>

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "heap.h"
#include "intr.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"  // FCNTL
#include "virtio.h"

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14

#define VIOBLK_BLKSZ 512 // added to be used later to access the vioblk size



//INTERNAL TYPE DEFINITIONS
//
struct virtio_blk_req{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t data[512];
    uint8_t status;
};
struct vioblk_storage {

    struct storage base;
    volatile struct virtio_mmio_regs * regs;

    int irqno;
    int instno;
    char opened;

    //char rq_buf[512];
    struct virtio_blk_req blk_rq;

    struct virtq_desc rq_desc;
    struct virtq_desc rq_indirect_desc[3];
    union {
        struct virtq_used used;
        char used_reserved[VIRTQ_USED_SIZE(1)];
    };
        union {
        struct virtq_avail avail;
        char avail_reserved[VIRTQ_AVAIL_SIZE(1)];
    };
     
    struct condition blk_cond ; // buffer wait condition variable
    uint16_t last_used ; // last used index

    unsigned int bytes_remaining ; // bytes_remaining is the number of bytes left in buffer
    
    struct lock blk_lock ; // lock 

};


// INTERNAL FUNCTION DECLARATIONS
//

/**
 * @brief Sets the virtq avail and virtq used queues such that they are available for use. (Hint,
 * read virtio.h) Enables the interupt line for the virtio device and sets necessary flags in vioblk
 * device.
 * @param sto Storage IO struct for the storage device
 * @return Return 0 on success or negative error code if error. If the given sto is already opened,
 * then return -EBUSY.
 */
static int vioblk_storage_open(struct storage* sto);

/**
 * @brief Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device. If
 * the given sto is not opened, this function does nothing.
 * @param sto Storage IO struct for the storage device
 * @return None
 */
static void vioblk_storage_close(struct storage* sto);

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf. Achieves this by
 * repeatedly setting the appropriate registers to request a block from the disk, waiting until the
 * data has been populated in block buffer cache, and then writes that data out to buf. Thread
 * sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the read within the VirtIO device
 * @param buf A pointer to the buffer to fill with the read data
 * @param bytecnt The number of bytes to read from the VirtIO device into the buffer
 * @return The number of bytes read from the device, or negative error code if error
 */
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Writes bytecnt number of bytes from the parameter buf to the disk. The size of the virtio
 * device should not change. You should only overwrite existing data. Write should also not create
 * any new files. Achieves this by filling up the block buffer cache and then setting the
 * appropriate registers to request the disk write the contents of the cache to the specified block
 * location. Thread sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the write within the VirtIO device
 * @param buf A pointer to the buffer with the data to write
 * @param bytecnt The number of bytes to write to the VirtIO device from the buffer
 * @return The number of bytes written to the device, or negative error code if error
 */
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions on the VirtIO block device.
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage IO struct for the storage device
 * @param op Operation to execute. vioblk should support FCNTL_GETEND.
 * @param arg Argument specific to the operation being performed
 * @return Status code on the operation performed
 */
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg);

/**
 * @brief The interrupt handler for the VirtIO device. When an interrupt occurs, the system will
 * call this function.
 * @param irqno The interrupt request number for the VirtIO device
 * @param aux A generic pointer for auxiliary data.
 * @return None
 */
static void vioblk_isr(int irqno, void* aux);

// INTERNAL GLOBAL VARIABLES
//
static const struct storage_intf vioblk_storage_intf = {
    .blksz = 512,
    .open = &vioblk_storage_open,
    .close = &vioblk_storage_close,
    .fetch = &vioblk_storage_fetch,
    .store = &vioblk_storage_store,
    .cntl = &vioblk_storage_cntl
};

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.
/**
 * @brief Initializes virtio block device with the necessary IO operation functions and sets the
 * required feature bits.
 * @param regs Memory mapped register of Virtio
 * @param irqno Interrupt request number of the device
 * @return None
 */
void vioblk_attach(volatile struct virtio_mmio_regs* regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_storage* vbd;
    unsigned int blksz;
    int result;

    trace("%s(regs=%p,irqno=%d)", __func__, regs, irqno);

    assert(regs->device_id == VIRTIO_ID_BLOCK);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize();  // fence o,io

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);

    if (result != 0) {
        //kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    /*
    2. If the VIRTIO_BLK_F_BLK_SIZE feature is negotiated, blk_size can be read to determine the optimal
    sector size for the driver to use. This does not affect the units used in the protocol (always 512 bytes),
    but awareness of the correct value can affect performance.
    */
    //so basically we can keep the const declaration of the vioblk_storage_intf.

    // FIXME
    vbd = kcalloc(1, sizeof(struct vioblk_storage));
    
    virtio_attach_virtq(regs, 0, 1, (uint64_t)&vbd->rq_desc, (uint64_t)&vbd->used, (uint64_t)&vbd->avail);
    
    vbd->regs = regs;
    vbd->irqno = irqno;
    vbd->opened = 0;
    //something about the request count


    // discovery of virtqueues and descriptors here
    vbd->rq_desc.addr = (uint64_t)vbd->rq_indirect_desc;
    vbd->rq_desc.flags = VIRTQ_DESC_F_INDIRECT;
    vbd->rq_desc.len = 3*sizeof(struct virtq_desc);
    vbd->rq_desc.next = -1;
    
    vbd->avail.ring[0] = 0;
    vbd->avail.idx = 0;

    //discovery of indirect descriptors 
    vbd->rq_indirect_desc[0].addr = (uint64_t)&vbd->blk_rq;
    vbd->rq_indirect_desc[0].flags = VIRTQ_DESC_F_NEXT;
    vbd->rq_indirect_desc[0].len = sizeof(uint32_t)*2 + sizeof(uint64_t);
    vbd->rq_indirect_desc[0].next = 1;

    vbd->rq_indirect_desc[1].addr = (uint64_t)&vbd->blk_rq.data;
    vbd->rq_indirect_desc[1].flags = VIRTQ_DESC_F_NEXT;
    vbd->rq_indirect_desc[1].len = 512* sizeof(char);
    vbd->rq_indirect_desc[1].next = 2;

    vbd->rq_indirect_desc[2].addr = (uint64_t)&vbd->blk_rq.status;
    vbd->rq_indirect_desc[2].flags = VIRTQ_DESC_F_WRITE;
    vbd->rq_indirect_desc[2].len = sizeof(uint8_t);
    vbd->rq_indirect_desc[2].next = -1;


    storage_init(&vbd->base, &vioblk_storage_intf, regs->config.blk.capacity); //capacity became present regardless of which feature bits were present
    kprintf("vbd->base.capacity: %d\n", vbd->base.capacity);
    
    vbd->instno = register_device(VIOBLK_NAME,DEV_STORAGE, vbd);
    regs->status |= VIRTIO_STAT_DRIVER_OK;

    //__sync_synchronize(); used in viorng_attach

}

static int vioblk_storage_open(struct storage* sto) {
    // FIXME
    if (sto == NULL ){ 
        return -EINVAL;
    }

    struct vioblk_storage * vbd = (void *) sto - offsetof(struct vioblk_storage, base);

    if (vbd->opened == 1) return -EBUSY;

    virtio_enable_virtq(vbd->regs,0);

    enable_intr_source(vbd->irqno, VIOBLK_INTR_PRIO, vioblk_isr, vbd);
    vbd->opened = 1;

    condition_init( &vbd->blk_cond , "blk_cond" ) ; // Storage 
    
    lock_init( &vbd->blk_lock ) ;

    return 0;
}

static void vioblk_storage_close(struct storage* sto) {
    // FIXME

    if (sto == NULL){ 
        return ;
    }

    struct vioblk_storage * vbd = (void *) sto - offsetof(struct vioblk_storage, base);
    if (vbd->opened ==0) return;

    virtio_reset_virtq(vbd->regs, 0);
    disable_intr_source(vbd->irqno);
    vbd->opened =0;
    return;
}

static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                unsigned long bytecnt) {
    trace("%s();",__func__);

    if (sto == NULL || buf == NULL ){ 
        return -EINVAL;
    }

    struct vioblk_storage * vbd = (void *)sto - offsetof(struct vioblk_storage, base);

    if (!vbd->opened) return -EINVAL;
    
    lock_acquire(&vbd->blk_lock);

    if (vbd->blk_rq.status != 0) {
        trace("vioblk_storage_fetch failed because of an invalid read request");
        lock_release(&vbd->blk_lock);
        return -EINVAL ;
    }
    
    unsigned long blksz = storage_blksz(sto);
    bytecnt = (bytecnt/blksz)*blksz;//rounds the bytecount down to the nearest blocksize
    unsigned long dev_size = vbd->regs->config.blk.capacity*VIOBLK_BLKSZ;
    if( pos >= dev_size || bytecnt < 1 )
    {
        trace("Arguments out of bounds for vioblk_storage_fetch") ;
        lock_release(&vbd->blk_lock);
        return 0;
    }
    if(pos + bytecnt > dev_size )
    {
        bytecnt = dev_size - pos;
    }
    vbd->rq_indirect_desc[1].flags  |= VIRTQ_DESC_F_WRITE ;
    vbd->blk_rq.type = 0;

    if( bytecnt == 0 ){
        lock_release(&vbd->blk_lock);
        return 0 ;
    }

    //lock_acquire(&vbd->blk_lock);

    vbd->bytes_remaining = 0 ;
    
    long maxcnt = 0;
    unsigned long long curr_pos = pos;
    while ( maxcnt < bytecnt ){

        int pie = disable_interrupts();
        vbd->blk_rq.sector = curr_pos / 512;
    
        char *out = buf;
        vbd->bytes_remaining = 0;

        uint16_t before = vbd->last_used; // Is this good ??????????

        vbd->avail.idx ++ ;
        //vbd->last_used++;
        virtio_notify_avail(vbd->regs, 0);
        
        while (vbd->last_used == before ) {
            trace("vbd->last_used: %d, before: %d\n", vbd->last_used, before);
            condition_wait(&vbd->blk_cond);
        }

        long offset =  curr_pos % VIOBLK_BLKSZ;


        long read_length;
        // if (offset > maxcnt){
        //     read_length = 512 - maxcnt;
        // } else {
        //     read_length = 512 - offset;
        // }
        read_length = MIN(512 - offset, bytecnt - maxcnt);

        // READ from start of buffer, otherwise error
        memcpy(out + maxcnt, vbd->blk_rq.data + offset, read_length);

        // decrement/increment
        maxcnt += read_length;
        curr_pos += read_length;
        vbd->bytes_remaining = 0;
        restore_interrupts(pie);

    }

    virtio_enable_virtq( vbd->regs , 0 ) ;
    lock_release(&vbd->blk_lock);
    // kprintf("return val for fetch: %d\n\n\n" , maxcnt) ;
    return maxcnt;
}

static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt) {
    // FIXME

    if (sto == NULL || buf == NULL ){ 
        return -EINVAL;
    }


    struct vioblk_storage * vbd = (void *)sto - offsetof(struct vioblk_storage, base);

    if (!vbd->opened) return -EINVAL;
    
    lock_acquire(&vbd->blk_lock);

    if (vbd->blk_rq.status != 0) {
        //kprintf("vioblk_storage_store failed because of an invalid read request");
        lock_release(&vbd->blk_lock) ;
        return -EBUSY ;
    }

    unsigned long blksz = storage_blksz(sto);
    bytecnt = (bytecnt/blksz)*blksz;//rounds the bytecount down to the nearest blocksize
    unsigned long dev_size = vbd->regs->config.blk.capacity*VIOBLK_BLKSZ;

        if (pos >= dev_size || bytecnt <= 0 ) {
            lock_release(&vbd->blk_lock) ;
            return 0;
        }

        if (pos + bytecnt > dev_size) {
            bytecnt = dev_size - pos;
        }

        vbd->rq_indirect_desc[1].flags  &= ~VIRTQ_DESC_F_WRITE ;
        vbd->blk_rq.type = 1;

        // if( bytecnt == 0 ){
        //     return 0 ;
        // }

        //vbd->bytes_remaining = 0 ;
    
        //lock_acquire(&vbd->blk_lock);
        
        long maxcnt = 0;
        unsigned long long curr_pos = pos;
        const void *out = buf ;

        while (maxcnt < bytecnt) {
            int pie = disable_interrupts();
            
            vbd->blk_rq.sector = curr_pos / 512;

            long offset =  curr_pos % VIOBLK_BLKSZ;

            long write_length;

            if ( (VIOBLK_BLKSZ - offset ) > ( bytecnt - maxcnt ) ){
                write_length = bytecnt - maxcnt ;
            } else {
                write_length = VIOBLK_BLKSZ - offset ;
            }

            memcpy( vbd->blk_rq.data + offset , out , write_length ) ;

            uint16_t before = vbd->last_used; // Is this good ??????????

            vbd->avail.idx ++ ;
            virtio_notify_avail( vbd->regs , 0 ) ;

            //uint16_t before = vbd->last_used; // Is this good ??????????
            while (vbd->last_used == before ) {
                condition_wait(&vbd->blk_cond);
            }

            maxcnt += write_length ;
            curr_pos += write_length ;

            restore_interrupts(pie);
        }

        virtio_enable_virtq(vbd->regs, 0) ;
        lock_release(&vbd->blk_lock);
        // kprintf("return val for store: %d\n\n\n" , maxcnt) ;
        return maxcnt ;
        //HERE
}

static int vioblk_storage_cntl(struct storage* sto, int op, void* arg) {
    // FIXME
    if ( sto == NULL ){ 
        return -EINVAL;
    }
    
    struct vioblk_storage * vbd = (void *)sto - offsetof(struct vioblk_storage, base);

    if (!vbd->opened) return -EINVAL;

    switch (op) {
        case FCNTL_GETEND:
            unsigned long blk_capacity = vbd->regs->config.blk.capacity ;
            unsigned long end_pos = blk_capacity*VIOBLK_BLKSZ ;
            trace("blk_capacity: %d\n end_pos: %d" , blk_capacity, end_pos);

            *(unsigned long *)arg = end_pos ; // arg = the capacity of the VirtIO block device in bytes
            return 0 ;

            break ;
        
        default:
            return -EINVAL ;
            break ;
    }

    return -ENOTSUP;
}

static void vioblk_isr(int irqno, void* aux) {
    // FIXME

    if ( aux == NULL ){ 
        return ;
    }

    struct vioblk_storage * const vbd = aux ;

    vbd->regs->interrupt_ack = vbd->regs->interrupt_status ;

    if( vbd->regs->interrupt_status == 1) {
        vbd->last_used = vbd->used.idx ; 
    }


    while ( vbd->used.idx != vbd->last_used )
    {
        vbd->last_used ++ ;
        vbd->bytes_remaining = VIOBLK_BLKSZ ;
    }

    condition_broadcast(&vbd->blk_cond);

    return;
}
