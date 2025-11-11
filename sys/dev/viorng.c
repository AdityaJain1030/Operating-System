// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "devimpl.h"
// #include "assert.h" hotfix deet
#include "misc.h"
#include "conf.h"
#include "intr.h"
#include "console.h"
// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "viorng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

// UART device structure



struct viorng_serial {
    struct serial base;
    volatile struct virtio_mmio_regs * regs;    // MMIO device registers

    int irqno;                               // interrupt number

    // VIRTIO specs say viorng (entropy) device has only one virtqueue
    struct virtq_desc* desc;                 // assume size one
    struct virtq_avail* avail;               // available ring
    struct virtq_used* used;                 // used ring, used ring contains used_elem which contain the used buffer element
    unsigned int virtqueue_size;                         // number of descriptors
    //unsigned int desc_indx;

    // end of virtqueue


    int opened;                               // tracks open/close state

    struct condition rand_number_ready; ///< signalled when rxbuf becomes not empty
    struct lock lock;
   
    void (*isr)(int irqno, void* aux);       // ISR function pointer
    void* aux;                                // auxiliary pointer for ISR
};

 // struct lock lk;                           // optional lock
    // struct condvar cv;                        // optional for CP3

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_serial_open(struct serial * ser);

static void viorng_serial_close(struct serial * ser);
static int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);

static void viorng_isr(int irqno, void * aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf viorng_serial_intf = {
    .blksz = 1,
    .open = &viorng_serial_open,
    .close = &viorng_serial_close,
    .recv = &viorng_serial_recv
};

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.



/* void viorng_attach(volatile struct virtio_mmio_regs *regs, int irqno)
 * Inputs:
 *    volatile struct virtio_mmio_regs *regs - Pointer to the MMIO register set for the VirtIO RNG device
 *    int irqno                              - IRQ number assigned to the device
 * Outputs: None
 * Return Value: None
 * Function: Attaches and initializes the VirtIO entropy (RNG) device. Sets up
 *            MMIO access, negotiates supported feature bits, and initializes
 *            the device’s virtqueue descriptors. The virtq_avail and virtq_used
 *            structures are attached using virtio_attach_virtq(). After the
 *            virtqueue and associated interfaces are initialized, the device
 *            is registered to the system
 * Side Effects:
 *    - Initializes hardware registers of the RNG device.
 *    - Allocates and sets up virtqueue structures.
 *    - Registers an interrupt handler for the given IRQ line.
 *    - Modifies global device registration state.
 */
void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct viorng_serial * vrng;
    int result;
    
    assert (regs->device_id == VIRTIO_ID_RNG);      // why didn't we check for magic number?

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    // fence o,io

    // 
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        // kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // Allocate and initialize device struct
    /*
    
    READ VIRTIO DOCS SECTION 4.2
    READ SECTION 2.7
    */
    // FIXME your code goes here 
    


    // allocate memory and intialize viorng serial struct
    vrng = kcalloc(1, sizeof(struct viorng_serial));
    vrng->irqno = irqno;
    vrng->regs = regs;
    vrng->opened = 0;


    // initialize conditions
    condition_init(&vrng->rand_number_ready, "viorng.rand_number_ready");
    lock_init(&vrng->lock);

    // allocate memory for descriptors
    // need to fill descriptor
    int queue_size = 1;
    uintptr_t ptr;
    ptr = (uintptr_t)kcalloc(16 * queue_size + 15, 1);  // add rounding trick, essentially allocate more memory, than add 15 and round down.
    vrng->desc = (void*)((ptr + 15) & ~15);

    ptr = (uintptr_t)kcalloc(6 + 2 * queue_size + 1, 1);
    vrng->avail = (void*)((ptr + 1) & ~1);

    ptr = (uintptr_t)kcalloc(6 + 8 * queue_size + 3, 1);
    vrng->used = (void*)((ptr + 3) & ~3);


    vrng->virtqueue_size = 1;   // set size of virtqueue to be 1
    // Tells device the location of each entry in the descriptor table
    virtio_attach_virtq(regs, 0, 1, (uint64_t)vrng->desc, (uint64_t)vrng->used, (uint64_t)vrng->avail);
    serial_init(&vrng->base, &viorng_serial_intf);          // Initialize the serial interface
    register_device(VIORNG_NAME, DEV_SERIAL, vrng);         // Register the device with the system so it can be found

    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();

}
/* int viorng_serial_open(struct serial *ser)
 * Inputs:
 *    struct serial *ser - Pointer to the serial interface associated with the VirtIO RNG device
 * Outputs: None
 * Return Value:
 *    0       - Success
 *   -EBUSY   - The specified serial interface is already open
 * Function: Initializes and enables the VirtIO RNG serial interface for use. 
 *            Makes the virtqueue 'avail' and 'used' rings available for data 
 *            exchange (see virtio.h for details). Enables the 
 *            interrupt source for the RNG device, registering the correct ISR
 * Side Effects:
 *    - Modifies the serial interface and virtqueue state.
 *    - Enables hardware interrupts for the RNG device.
 *    - Configures the PLIC by enabling this interrupt source
 */
int viorng_serial_open(struct serial * ser) {
    struct viorng_serial * const viorng =
        (void*)ser - offsetof(struct viorng_serial, base);
    lock_acquire(&viorng->lock);
    if (viorng->opened) {
        lock_release(&viorng->lock);
        return -EBUSY;
    }
    virtio_enable_virtq(viorng->regs, 0);                           // specifies which virtqueue to enable
    viorng->opened = 1;                                             // set opened member to 1
    enable_intr_source(viorng->irqno, VIORNG_IRQ_PRIO, viorng_isr, viorng);
    lock_release(&viorng->lock);
    return 0;
}
/* void viorng_serial_close(struct serial *ser)
 * Inputs:
 *    struct serial *ser - Pointer to the serial interface associated with the VirtIO RNG device
 * Outputs: None
 * Return Value: None
 * Function: Closes the VirtIO RNG serial interface. Resets the virtqueue 'avail' 
 *            and 'used' rings, and disables the interrupt source for the device 
 *            to prevent further interrupt handling. If the specified viorng
 *            is not currently open, the function performs no action.
 * Side Effects:
 *    - Resets virtqueue state.
 *    - Disables device interrupts by configuring the PLIC
 *    - Modifies the serial interface’s internal status flags.
 */
void viorng_serial_close(struct serial * ser) {
     struct viorng_serial * const viorng =
        (void*)ser - offsetof(struct viorng_serial, base);
    lock_acquire(&viorng->lock);
    if (viorng->opened == 0) {
        lock_release(&viorng->lock);
        return;
    }
    virtio_reset_virtq(viorng->regs, 0);
    disable_intr_source(viorng->irqno);
    viorng->opened = 0;
    lock_release(&viorng->lock);
}
/* int viorng_serial_recv(struct serial *ser, void *buf, unsigned int bufsz)
 * Inputs:
 *    struct serial *ser  - Pointer to the serial interface for the VirtIO RNG device
 *    void *buf           - Pointer to the destination buffer for received random data
 *    unsigned int bufsz  - Maximum number of bytes to read into buf
 * Outputs:
 *    buf - Filled with up to bufsz bytes of random data
 * Return Value:
 *    int len   - Number of bytes of randomness successfully read
 *    -EINVAL   - The specified serial interface is not currently open
 * Function: Reads up to bufsz bytes of random data from the VirtIO Entropy (RNG) device
 *            and writes them into the provided buffer. The function initiates a request
 *            for entropy by setting the appropriate device registers and waits until the
 *            requested data becomes available. For CP2, a spin-wait may be used; for CP3,
 *            this should be implemented using a condition variable for synchronization.
 * Side Effects:
 *    - Initiates VioRNG device operations.
 *    - May suspend the calling thread (depending on implementation).
 *    - Modifies virtqueue and buffer descriptors associated with the VioRNG device
 */
int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    struct viorng_serial * const viorng =
        (void*)ser - offsetof(struct viorng_serial, base);
    lock_acquire(&viorng->lock);
    if (viorng->opened == 0)
    {
        lock_release(&viorng->lock);
        return -EINVAL;
    }
    if (bufsz <= 0) {
        lock_release(&viorng->lock);
        return 0;
    }
    volatile uint16_t oldIndex = viorng->used->idx;
    //  set the length to how many bytes we want to read, viorng device will look at this when it generates random numbers
    viorng->desc[0].addr  = (uintptr_t)buf;
    viorng->desc[0].len   = bufsz;                    
    viorng->desc[0].flags = VIRTQ_DESC_F_WRITE;       // since we are creating a random number generator
    viorng->desc[0].next  = 0;   
    viorng->avail->ring[0] = 0;


    viorng->avail->idx++;
    // request random number from device
    // used_idx is a FREE RUNNING COUNTER!
    virtio_notify_avail(viorng->regs, 0);           // notifies the queue


    // Critical Section: Index may be modified before condition_wait
    int pie = disable_interrupts();
    while (oldIndex == viorng->used->idx)   //   spin wait
    {
        condition_wait(&viorng->rand_number_ready);
    }
    restore_interrupts(pie);
    uint32_t len = viorng->used->ring[oldIndex % viorng->virtqueue_size].len;
    lock_release(&viorng->lock);
    return len;
}
/* void viorng_isr(int irqno, void *aux)
 * Inputs:
 *    int irqno  - IRQ number corresponding to the VirtIO RNG device interrupt
 *    void *aux  - Auxiliary data passed to the interrupt handler (typically the device or serial struct)
 * Outputs: None
 * Return Value: None
 * Function: Interrupt Service Routine (ISR) for the VirtIO RNG device. Handles
 *            completion of entropy requests by acknowledging the interrupt and 
 *            setting the appropriate device registers to clear the interrupt condition.
 *            Wakes the threads waiting for RNG data to become available
 * Side Effects:
 *    - Modifies device registers to acknowledge and clear interrupts.
 *    - Wakes or unblocks threads waiting for RNG data by broadcasting a condition variable
 */
void viorng_isr(int irqno, void * aux) {
    struct viorng_serial* vrng = (struct viorng_serial *) aux;
    // check the interruptStatus register, check to see if it is 1, if not, then it is configuration change and return
    // bit 0 is used buffer notification bit, should be set
    if ((vrng->regs->interrupt_status & 1) == 0)
    {
        return;
    }
    vrng->regs->interrupt_ack = 1;                      // Documenation requries ACK bit to be set
    condition_broadcast(&(vrng->rand_number_ready));    // Wakes up any threads waiting for random number
    return;

}