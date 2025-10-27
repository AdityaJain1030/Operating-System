/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌‍‌⁠​⁠⁠‌
    @brief VirtIO block device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "devimpl.h"
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

//VIOBLK device structure

struct vioblk_storage {
    struct storage base;
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    char opened;

    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    unsigned int queue_size;
    uint16_t next_desc_idx;

    struct {
        uint32_t type;
        uint32_t reserved;
        uint64_t sector;
    } req_header;
    uint8_t req_status;

    struct condition ready;
    struct lock lock;
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

static const struct storage_intf vioblk_storage_intf = {
    .blksz = 512,
    .open = &vioblk_storage_open,
    .close = &vioblk_storage_close,
    .fetch = &vioblk_storage_fetch,
    .store = &vioblk_storage_store,
    .cntl = &vioblk_storage_cntl
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
    blk = kcalloc(1, sizeof(*blk));
    blk->regs = regs;
    blk->irqno = irqno;
    blk->opened = 0;
    condition_init(&blk->ready, "vioblk.ready");
    lock_init(&blk->lock);


    regs->queue_sel = 0;
    blk->queue_size = 32;
    blk->next_desc_idx = 0;
    
    uintptr_t ptr;
    ptr = (uintptr_t)kcalloc(16 * blk->queue_size + 15, 1);
    blk->desc = (void*)((ptr + 15) & ~15);

    ptr = (uintptr_t)kcalloc(6 + 2 * blk->queue_size + 1, 1);
    blk->avail = (void*)((ptr + 1) & ~1);

    ptr = (uintptr_t)kcalloc(6 + 8 * blk->queue_size + 3, 1);
    blk->used = (void*)((ptr + 3) & ~3);

    regs->queue_num = blk->queue_size;
    virtio_attach_virtq(regs, 0, blk->queue_size, (uint64_t)blk->desc, (uint64_t)blk->used, (uint64_t)blk->avail);

    struct storage_intf* intf = kmalloc(sizeof(*intf));
    *intf = vioblk_storage_intf;
    intf->blksz = blksz;

    long long capacity = (long long)regs->config.blk.capacity * 512;
    storage_init(&blk->base, intf, capacity);

    register_device(VIOBLK_NAME, DEV_STORAGE, &blk->base);

    regs->status |= VIRTIO_STAT_DRIVER_OK;
}

static int vioblk_storage_open(struct storage* sto) {
    if (sto == NULL) return -EINVAL;
    struct vioblk_storage * const blk =
        (void*)sto - offsetof(struct vioblk_storage, base);

    if (blk->opened) return -EBUSY;

    //figure out what device stuff we need to do
    blk->opened = 1;
    enable_intr_source(blk->irqno, VIORNG_INTR_PRIO, &vioblk_isr, blk);
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

    // kfree((void*)blk->desc);
    // kfree((void*)blk->avail);
    // kfree((void*)blk->used);
    // kfree((void*)sto->intf);
}

static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt) {
    // FIXME
    return -ENOTSUP;
}

static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt) {
    // FIXME
    return -ENOTSUP;
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