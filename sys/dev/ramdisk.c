/*! @file ramdisk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌‍‌⁠​⁠⁠‌
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
    struct storage storage;  ///< Storage struct of memory storage
    void *buf;               ///< Block of memory
    size_t size;             ///< Size of memory block
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
    .blksz = 1,
    .open = &ramdisk_open,
    .close = &ramdisk_close,
    .fetch = &ramdisk_fetch,
    .store = NULL,  // Read-only storage (blob data in .rodata)
    .cntl = &ramdisk_cntl};

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Creates and registers a memory-backed storage device
 * @return None
 */
void ramdisk_attach() {
    // External symbols from linker script for embedded blob data
    extern char _kimg_blob_start[], _kimg_blob_end[];
    struct ramdisk *rd;
    size_t sz = (size_t)(_kimg_blob_end - _kimg_blob_start);
    rd = kcalloc(1, sizeof(*rd));
    if (rd == NULL) return; /* out of memory, silently skip registering */
    /* If the embedded blob is empty, don't register the device. */
    if (sz == 0) {
        kfree(rd); /* nothing to register */
        return; /*CHANGED: avoid registering a zero-size ramdisk */
    }

    /* Point to the linker-provided blob (no copy). The blob should be
     * located in the binary image (read-only region). */
    rd->buf = (void *)_kimg_blob_start;
    rd->size = sz;

    /* Initialize the generic storage fields (intf pointer and capacity). */
    storage_init(&rd->storage, &ramdisk_intf, (unsigned long long)sz);

    /* Register the device so higher-level code can find it by name. */
    register_device(RAMDISK_NAME, DEV_STORAGE, rd);
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Opens the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 * @return 0 on success
 */
static int ramdisk_open(struct storage *sto) {
    struct ramdisk *rd;

    if (sto == NULL) return -EINVAL;

    /* storage is the first member; cast back to ramdisk */
    rd = (struct ramdisk *)sto;
    /* The `storage` object is embedded at the start of `struct ramdisk`,
     * so the caller passes a pointer to that embedded `storage`. Casting
     * lets us access ramdisk-specific fields if needed; no init required. */
    (void)rd; /* nothing to initialize for ramdisk */
    /*CHANGED: explicitly return negative error code on invalid input above; otherwise success */
    return 0;
}

/**
 * @brief Closes the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 */
static void ramdisk_close(struct storage *sto) {
    /*CHANGED: if sto is NULL, return immediately (defensive). No resources to
     * release for the embedded blob-backed ramdisk otherwise. */
    if (sto == NULL) return;
    return;
}

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf.
 * @details Performs proper bounds checks, then copies data from memory block to passed buffer
 * @param sto Storage struct pointer for memory storage
 * @param pos Position in storage to read from
 * @param buf Buffer to copy data from memory to
 * @param bytecnt Number of bytes to read from memory
 * @return Number of bytes successfully read
 */
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt) {
    struct ramdisk *rd;
    unsigned long long avail;
    unsigned long long tocopy;

    if (sto == NULL || buf == NULL) return -EINVAL;

    rd = (struct ramdisk *)sto;

    if (pos >= rd->size) return 0; /* EOF */

    avail = rd->size - pos;
    tocopy = (unsigned long long)bytecnt;
    if (tocopy > avail) tocopy = avail;

    /* Copy from the blob into the caller's buffer. The device uses
     * byte-granular addressing (blksz==1), so pos is a byte offset. */
    memcpy(buf, (char *)rd->buf + pos, (size_t)tocopy);

    /* Return the number of bytes delivered; 0 indicates EOF. */
    return (long)tocopy;
}

/**
 * @brief _cntl_ functions for memory storage.
 * @details Memory storage supports basic control operations
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage struct pointer for memory storage
 * @param cmd command to execute. ramdisk should support FCNTL_GETEND.
 * @param arg Argument for commands
 * @return 0 on success, error on failure or unsupported command
 */
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg) {
    if (sto == NULL) return -EINVAL;

    /* Many control operations write results via pointers passed in `arg`.
     * FCNTL_GETEND expects an unsigned long long* pointing to receive the
     * device capacity in bytes. */
    switch (cmd) {
    case FCNTL_GETEND: {
        unsigned long long *endp = (unsigned long long *)arg;
        if (endp == NULL) return -EINVAL;
        *endp = sto->capacity; /* write capacity through caller pointer */
        return 0;
    }
    case FCNTL_MMAP:
        /* mmap not supported for the embedded blob-backed ramdisk */
        kprintf("MMAP is not a supported yet\n");
        return -ENOTSUP;
    default:
        return -ENOTSUP;
    }
}
