/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍Pillaging-Koala-Pink-Giraffe
    @brief VirtIO block device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "devimpl.h"
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

// VirtIO block request types
#define VIRTIO_BLK_T_IN 0  // Read
#define VIRTIO_BLK_T_OUT 1 // Write

// VirtIO block request status
#define VIRTIO_BLK_S_OK 0

//VIOBLK device structure

struct vioblk_header
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

struct vioblk_storage {
    struct storage base;
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    int opened;                             // tracks open/close state

    //pointers for alignment, we dont clean these up
    struct virtq_desc* desc;                 // assume size one
    struct virtq_avail* avail;               // available ring
    struct virtq_used* used;                 // used ring
    unsigned int virtqueue_size;             // always 3

    struct condition ready;                  //< signalled when ready
    struct lock lock;

    // protect these with lock as well
    struct vioblk_header header;
    uint8_t status; // 5.2.4

};

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

// INTERNAL GLOBAL INTERFACES 

static int vioblk_storage_open(struct storage* sto);
static void vioblk_storage_close(struct storage* sto);
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt);
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt);
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg);
static void vioblk_isr(int irqno, void* aux);

// INTERNAL GLOBAL VARIABLES
//

// INTERNAL FUNCTION DECLARATIONS
//

static void fill_descriptor_table(struct virtq_desc* desc, const struct vioblk_header* header, const void * buf, const unsigned long bytecnt, const uint8_t* status, const int is_read)
{
    // Fill descriptor 1 (header)
    desc[0].addr = (uintptr_t)header;
    desc[0].len = sizeof(struct vioblk_header);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;

    // Fill descriptor 2 (data)
    desc[1].addr = (uintptr_t)buf;
    desc[1].len = bytecnt;
    desc[1].flags = VIRTQ_DESC_F_NEXT;
    if (is_read) {
        // Device will WRITE to this buffer
        desc[1].flags |= VIRTQ_DESC_F_WRITE;
    }
    desc[1].next = 2;

    // Fill descriptor 3 (status)
    desc[2].addr = (uintptr_t)status;
    desc[2].len = sizeof(uint8_t); // 5.2.5 only one byte long
    desc[2].flags = VIRTQ_DESC_F_WRITE; // next is not set and write is
}

static struct storage_intf* get_vioblk_storage_intf(int blksz)
{
    struct storage_intf* out = kmalloc(sizeof(struct storage_intf));
    out->blksz = blksz;
    out->open = &vioblk_storage_open;
    out->close = &vioblk_storage_close;
    out->fetch = &vioblk_storage_fetch;
    out->store = &vioblk_storage_store;
    out->cntl = &vioblk_storage_cntl;

    return out;
}

static int vioblk_storage_open(struct storage* sto) {
    if (sto == NULL) return -EINVAL;
    struct vioblk_storage * const blk =
        (void*)sto - offsetof(struct vioblk_storage, base);

    if (blk->opened) return -EBUSY;

    //figure out what device stuff we need to do
    blk->opened = 1;
    enable_intr_source(blk->irqno, VIORNG_INTR_PRIO, &vioblk_isr, blk);
    
    // Write 0x1 to QueueReady.
    virtio_enable_virtq(blk->regs, 0);
    // FIXME your code goes here
    return 0;
}

static void vioblk_storage_close(struct storage* sto) {
    if (sto == NULL) return;
    struct vioblk_storage * const blk =
        (void*)sto - offsetof(struct vioblk_storage, base);

    if (!blk->opened) return;
    blk->opened = 0;
    disable_intr_source(blk->irqno); //disable in plic
    virtio_reset_virtq(blk->regs, 0); //reset virtq, dunno what it sets lowks

    // we should not free in case of re-open
    // kfree((void*)blk->desc);
    // kfree((void*)blk->avail);
    // kfree((void*)blk->used);
    // kfree((void*)sto->intf);
}

static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt) {
    // FIXME
    struct vioblk_storage * const blk =
        (void*)sto - offsetof(struct vioblk_storage, base);

    if (!blk->opened) return -EINVAL;
    if (bytecnt == 0) return 0;

    if (pos > sto->capacity) return -EINVAL;
    
    //fill header
    uint64_t sector = pos / 512; // 5.2.6
    blk->header.sector = sector;
    blk->header.type = VIRTIO_BLK_T_IN;

    // [10/21 07:04] For vioblk_storage_store and vioblk_storage_fetch, writes & reads that exceed the end of the block device should be truncated. Do not return a negative error code in these scenarios.
    if (pos + bytecnt > sto->capacity) bytecnt = sto->capacity - pos;
    
    // [10/21 15:17] In vioblk_storage_store and vioblk_storage_fetch, writes & reads whose bytecnt are not a multiple of blksz should be rounded down to the nearest blksz. The number of bytes written & read should equal the return value.
    bytecnt /= sto->intf->blksz;
    bytecnt *= sto->intf->blksz;

    if (bytecnt == 0) return 0; // for bytecnt < 512

    lock_acquire(&blk->lock);

    // fill descriptor table
    fill_descriptor_table(blk->desc, &blk->header, buf, bytecnt, &blk->status, 1);
    // Add message to avail ring
    blk->avail->ring[blk->avail->idx % blk->virtqueue_size] = 0;
    __sync_synchronize(); // 2.7.13
    blk->avail->idx++;
    __sync_synchronize(); // 2.7.13
    
    // Notify Device
    virtio_notify_avail(blk->regs, 0);
    
    // wait for response
    int pie = disable_interrupts();
    while (blk->avail->idx != blk->used->idx) 
        condition_wait(&blk->ready);
    restore_interrupts(pie);

    lock_release(&blk->lock);
    return bytecnt;
}

static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt) {
    // FIXME
    struct vioblk_storage * const blk =
        (void*)sto - offsetof(struct vioblk_storage, base);

    if (!blk->opened) return -EINVAL;
    if (bytecnt == 0) return 0;
    if (pos > sto->capacity) return -EINVAL;

    // [10/21 15:17] In vioblk_storage_store and vioblk_storage_fetch, writes & reads whose bytecnt are not a multiple of blksz should be rounded down to the nearest blksz. The number of bytes written & read should equal the return value.
    bytecnt /= sto->intf->blksz;
    bytecnt *= sto->intf->blksz;
    
    //fill header
    uint64_t sector = pos / 512; // 5.2.6
    blk->header.sector = sector;
    blk->header.type = VIRTIO_BLK_T_OUT;

    // [10/21 07:04] For vioblk_storage_store and vioblk_storage_fetch, writes & reads that exceed the end of the block device should be truncated. Do not return a negative error code in these scenarios.
    if (pos + bytecnt > sto->capacity) bytecnt = sto->capacity - pos;
    
    lock_acquire(&blk->lock);

    // fill descriptor table
    fill_descriptor_table(blk->desc, &blk->header, buf, bytecnt, &blk->status, 0);
    // Add message to avail ring
    blk->avail->ring[blk->avail->idx % blk->virtqueue_size] = 0;
    __sync_synchronize(); // 2.7.13
    blk->avail->idx++;
    __sync_synchronize(); // make sure everything is updated before sending data
    // Notify Device
    virtio_notify_avail(blk->regs, 0);
    
    // wait for response
    int pie = disable_interrupts();
    while (blk->avail->idx != blk->used->idx) 
        condition_wait(&blk->ready);
    restore_interrupts(pie);

    lock_release(&blk->lock);
    return bytecnt;
}

static int vioblk_storage_cntl(struct storage* sto, int op, void* arg) {
    // FIXME
    struct vioblk_storage * const blk = (void*)sto - offsetof(struct vioblk_storage, base);

    if (op == FCNTL_GETEND) {
        if (arg == NULL) return -EINVAL;
        *(unsigned long long*)arg = blk->base.capacity;
        return 0;
    }
    else {
        return -ENOTSUP;
    }
}

static void vioblk_isr(int irqno, void* aux) {
    struct vioblk_storage * const blk = aux;
    blk->regs->interrupt_ack = blk->regs->interrupt_status;

    condition_broadcast(&blk->ready);
}

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
    struct vioblk_storage* blk;
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
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // The driver will typically initialize the virtual queue in the following way:

    // Select the queue writing its index (first queue is 0) to QueueSel.
    // Check if the queue is not already in use: read QueueReady, and expect a returned value of zero (0x0).
    // Read maximum queue size (number of elements) from QueueNumMax. If the returned value is zero (0x0) the queue is not available.
    // Allocate and zero the queue memory, making sure the memory is physically contiguous.
    // Notify the device about the queue size by writing the size to QueueNum.
    // Write physical addresses of the queue’s Descriptor Area, Driver Area and Device Area to (respectively) the QueueDescLow/QueueDescHigh, QueueDriverLow/QueueDriverHigh and QueueDeviceLow/QueueDeviceHigh register pairs.
    // Write 0x1 to QueueReady.

    //allocate driver memory
    blk = kcalloc(1, sizeof(*blk));
    blk->regs = regs;
    blk->irqno = irqno;
    blk->opened = 0;
    condition_init(&blk->ready, "vioblk.ready");
    lock_init(&blk->lock);
    blk->virtqueue_size = 3;
    
    // Select the queue writing its index (first queue is 0) to QueueSel.
    regs->queue_sel = 0;
    
    // Check if the queue is not already in use: read QueueReady, and expect a returned value of zero (0x0).
    if (regs->queue_ready != 0) {
        kprintf("regs->queue_ready is nonzero, queue already in use. initialization failed\n");
        return;
    }
    // Read maximum queue size (number of elements) from QueueNumMax. If the returned value is zero (0x0) the queue is not available.
    if (regs->queue_num_max == 0) {
        kprintf("regs->queue_num_max is zero, queue already in use. initialization failed\n");
        return;
    }
    
    // blk->next_desc_idx = 0;
    
    // Allocate and zero the queue memory, making sure the memory is physically contiguous.
    //altg alex lin the goat 
    uintptr_t ptr;
    ptr = (uintptr_t)kcalloc(16 * (blk->virtqueue_size) + 15, 1);
    blk->desc = (void*)((ptr + 15) & ~15);

    ptr = (uintptr_t)kcalloc(6 + 2 * blk->virtqueue_size + 1, 1);
    blk->avail = (void*)((ptr + 1) & ~1);

    ptr = (uintptr_t)kcalloc(6 + 8 * blk->virtqueue_size + 3, 1);
    blk->used = (void*)((ptr + 3) & ~3);

    // Notify the device about the queue size by writing the size to QueueNum.
    // Write physical addresses of the queue’s Descriptor Area, Driver Area and Device Area to (respectively) the QueueDescLow/QueueDescHigh, QueueDriverLow/QueueDriverHigh and QueueDeviceLow/QueueDeviceHigh register pairs.
    virtio_attach_virtq(regs, 0, blk->virtqueue_size, (uint64_t)blk->desc, (uint64_t)blk->used, (uint64_t)blk->avail);

    // driver init and registration
    long long capacity = (long long)regs->config.blk.capacity * 512; // 5.2.4
    storage_init(&blk->base, get_vioblk_storage_intf(blksz), capacity);
    register_device(VIOBLK_NAME, DEV_STORAGE, &blk->base);

    regs->status |= VIRTIO_STAT_DRIVER_OK;
}