/*! @file ramdisk.c
    @brief Memory-backed storage implementation
    @copyright Copyright (c) 2024-2025 University of Illinois
*/

#ifdef RAMDISK_DEBUG
#define DEBUG
#endif

#ifdef RAMDISK_TRACE
#define TRACE
#endif

#include <stddef.h>

#include "console.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "uio.h"
#include <limits.h>
#ifndef RAMDISK_NAME
#define RAMDISK_NAME "ramdisk"
#endif

// INTERNAL TYPE DEFINITIONS
//

/**
 * @brief Storage device backed by a block of memory. The implementation here
 * uses an embedded blob in the kernel image (read-only .rodata). The device
 * is therefore treated as read-only (no store implementation).
 */
struct ramdisk {
    struct storage storage;  ///< Storage struct of memory storage (must be first)
    void *buf;               ///< Block of memory
    size_t size;             ///< Size of memory block
    int opened;              ///< Track open state
};

// INTERNAL FUNCTION DECLARATIONS
//

static int ramdisk_open(struct storage *sto);
static void ramdisk_close(struct storage *sto);
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt);
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg);

// INTERNAL GLOBAL CONSTANTS
//

static const struct storage_intf ramdisk_intf = {
    .blksz = 512,  // fixed: use more Standard block size to match filesystem expectations
    .open = &ramdisk_open,
    .close = &ramdisk_close,
    .fetch = &ramdisk_fetch,
    .store = NULL,  // Read-only storage (blob data in .rodata)
    .cntl = &ramdisk_cntl
};

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Creates and registers a memory-backed storage device
 * @return None
 */
void ramdisk_attach() {
    extern char _kimg_blob_start[], _kimg_blob_end[];
    struct ramdisk *rd;
    size_t sz;

    trace("%s()", __func__);

    /* Calculate blob size */
    sz = (size_t)(_kimg_blob_end - _kimg_blob_start);
    
    /* Don't register if no blob present */
    if (sz == 0) {
        debug("ramdisk_attach: No blob data available");
        return; /* No blob to attach */
    }

    debug("ramdisk_attach: Found blob of size %lu bytes", (unsigned long)sz);

    /* Allocate ramdisk structure */
    rd = kcalloc(1, sizeof(*rd));
    if (rd == NULL) {
        /* Log error but don't panic */
        kprintf("ramdisk_attach: Failed to allocate memory\n");
        return;
    }

    /* Initialize ramdisk with blob data */
    rd->buf = (void *)_kimg_blob_start;
    rd->size = sz;
    rd->opened = 0;

    /* Initialize storage interface with actual size */
    storage_init(&rd->storage, &ramdisk_intf, (unsigned long long)sz);

    /* Register the device */
    if (register_device(RAMDISK_NAME, DEV_STORAGE, rd) != 0) {
        kfree(rd);
        kprintf("ramdisk_attach: Failed to register device\n");
    } else {
        debug("ramdisk_attach: Successfully registered ramdisk device");
    }
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Opens the ramdisk device
 * @param sto Storage struct pointer for memory storage
 * @return 0 on success, negative error code if error
 */
static int ramdisk_open(struct storage *sto) {
    struct ramdisk *rd;

    if (sto == NULL) return -EINVAL;
    // can we just delete the rest of this function?
    /* storage is the first member; cast back to ramdisk */
    rd = (struct ramdisk *)sto;
    
    /* Check if already opened */
    if (rd->opened) {
        return -EBUSY;
    }
    
    /* Validate ramdisk state */
    if (rd->buf == NULL || rd->size == 0) {
        return -EINVAL;
    }
    
    rd->opened = 1;
    
    debug("ramdisk_open: Opened ramdisk, size=%lu", (unsigned long)rd->size);
    
    return 0;
}

/**
 * @brief Closes the ramdisk device
 * @param sto Storage struct pointer for memory storage
 * @return None
 */
static void ramdisk_close(struct storage *sto) {
    struct ramdisk *rd;

    trace("%s(%p)", __func__, sto);

    if (sto == NULL) {
        return;
    }
    
    rd = (struct ramdisk *)sto;
    rd->opened = 0;
    
    debug("ramdisk_close: Closed ramdisk");
}

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf
 * @param sto Storage struct pointer for memory storage
 * @param pos Position in storage to read from
 * @param buf Buffer to copy data from memory to
 * @param bytecnt Number of bytes to read from memory
 * @return Number of bytes successfully read, negative error code on error
 */
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt) {
    struct ramdisk *rd;
    unsigned long long avail;
    unsigned long to_copy;

    trace("%s(%p, %llu, %p, %lu)", __func__, sto, pos, buf, bytecnt);

    /* Validate parameters */
    if (sto == NULL || buf == NULL) {
        return -EINVAL;
    }

    rd = (struct ramdisk *)sto; // make sure not to edit ordering of ramdisk pretty please

    /* Check if device is opened */
    if (!rd->opened) {
        return -EINVAL;
    }

    /* Validate ramdisk state */
    if (rd->buf == NULL || rd->size == 0) {
        return -EINVAL;
    }

    /* Handle zero-length read */
    if (bytecnt == 0) {
        return 0;
    }

    /* Check for EOF */
    if (pos >= rd->size) {
        debug("ramdisk_fetch: Read past EOF (pos=%llu, size=%lu)", 
              pos, (unsigned long)rd->size);
        return 0; /* EOF */
    }

    /* Calculate available bytes */
    avail = rd->size - pos;
    
    /* Determine copy size */
    to_copy = bytecnt;
    if (to_copy > avail) {
        to_copy = (unsigned long)avail;
    }

    /* Ensure to_copy fits in long for return value */
    if (to_copy > LONG_MAX) {
        to_copy = LONG_MAX;
    }

    /* Perform the copy */
    memcpy(buf, (char *)rd->buf + pos, to_copy);

    debug("ramdisk_fetch: Read %lu bytes from pos %llu", to_copy, pos);

    return (long)to_copy;
}

/**
 * @brief Control functions for memory storage
 * @param sto Storage struct pointer for memory storage
 * @param cmd Command to execute. ramdisk should support FCNTL_GETEND
 * @param arg Argument for commands
 * @return 0 on success, negative error code on failure
 */
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg) {
    struct ramdisk *rd;

    trace("%s(%p, %d, %p)", __func__, sto, cmd, arg);

    if (sto == NULL) {
        return -EINVAL;
    }

    rd = (struct ramdisk *)sto;

    /* Check if device is opened */
    if (!rd->opened) {
        return -EINVAL;
    }

    // switch statements make the asm a bit harder to debug wrt locks n stuff... may refactor to if as we dont need to support more 
    // misc commands
    // if (cmd == FCNTL_GETEND)
    // {
    //     if (arg == NULL) return -EINVAL;
    //     if (rd->) .. do stuff here. We dont need to support any other switches 
    // }
    // else {
    //     kprintf("Operation %d is not supported yet\n", cmd);
    //     return -EINVAL;
    // }
    switch (cmd) {
    case FCNTL_GETEND: {
        if (arg == NULL) {
            return -EINVAL;
        }
        
        /* Return the actual size/capacity of the ramdisk */
        *(unsigned long long *)arg = rd->storage.capacity;
        
        debug("ramdisk_cntl: FCNTL_GETEND returns %llu", rd->storage.capacity);
        /* Verify storage capacity is valid */
        if (rd->storage.capacity == 0 || rd->storage.capacity > rd->size) { // shouldnt we return zero here for cap = 0?
            return -EINVAL;
        }
        
        return 0;
    }
    
    case FCNTL_MMAP:
        /* Not supported per requirements */
        kprintf("MMAP is not supported yet\n");
        return -ENOTSUP;
        
    default:
        /* Unsupported command */
        debug("ramdisk_cntl: Unsupported command %d", cmd);
        return -ENOTSUP;
    }
}