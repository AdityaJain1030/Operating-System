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

struct ramdisk {
    struct storage storage;
    void *buf;
    size_t size;
    int opened;
};

// INTERNAL FUNCTION DECLARATIONS

static int ramdisk_open(struct storage *sto);
static void ramdisk_close(struct storage *sto);
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt);
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg);

// INTERNAL GLOBAL CONSTANTS

static const struct storage_intf ramdisk_intf = {
    .blksz = 1, // KEEP THIS 1
    .open = &ramdisk_open,
    .close = &ramdisk_close,
    .fetch = &ramdisk_fetch,
    .store = NULL,
    .cntl = &ramdisk_cntl
};

// EXPORTED FUNCTION DEFINITIONS

void ramdisk_attach() {
    extern char _kimg_blob_start[], _kimg_blob_end[];
    struct ramdisk *rd;
    size_t sz;

    trace("%s()", __func__);

    sz = (size_t)(_kimg_blob_end - _kimg_blob_start);
    
    if (sz == 0) {
        kprintf("ramdisk_attach: No blob data available\n");
        return; /* No blob to attach */
    }

    kprintf("ramdisk_attach: Found blob of size %lu bytes\n", (unsigned long)sz);

    rd = kcalloc(1, sizeof(*rd));
    if (rd == NULL) {
        kprintf("ramdisk_attach: Failed to allocate memory\n");
        return;
    }

    rd->buf = (void *)_kimg_blob_start;
    rd->size = sz;
    rd->opened = 0;

    storage_init(&rd->storage, &ramdisk_intf, (unsigned long long)sz);

    if (register_device(RAMDISK_NAME, DEV_STORAGE, rd) != 0) {
        kfree(rd);
        kprintf("ramdisk_attach: Failed to register device\n");
    } else {
        kprintf("ramdisk_attach: Successfully registered ramdisk device\n");
    }
}

// INTERNAL FUNCTION DEFINITIONS

static int ramdisk_open(struct storage *sto) {
    struct ramdisk *rd;

    trace("%s(%p)", __func__, sto);

    if (sto == NULL) {
        return -EINVAL;
    }

    rd = (struct ramdisk *)sto;
    
    if (rd->buf == NULL || rd->size == 0) {
        return -EINVAL;
    }
    
    rd->opened = 1;
    
    debug("ramdisk_open: Opened ramdisk, size=%lu", (unsigned long)rd->size);
    
    return 0;
}

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

static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt) {
    struct ramdisk *rd;
    unsigned long long avail;
    unsigned long to_copy;

    trace("%s(%p, %llu, %p, %lu)", __func__, sto, pos, buf, bytecnt);

    if (sto == NULL || buf == NULL) {
        return -EINVAL;
    }

    rd = (struct ramdisk *)sto;

    if (!rd->opened) {
        return -EINVAL;
    }

    if (rd->buf == NULL || rd->size == 0) {
        return -EINVAL;
    }

    if (bytecnt == 0) {
        return 0;
    }

    if (pos >= rd->size) {
        debug("ramdisk_fetch: Read past EOF (pos=%llu, size=%lu)", 
              pos, (unsigned long)rd->size);
        return 0;
    }

    avail = rd->size - pos;
    
    to_copy = bytecnt;
    if (to_copy > avail) {
        to_copy = (unsigned long)avail;
    }

    if (to_copy > LONG_MAX) {
        to_copy = LONG_MAX;
    }

    memcpy(buf, (char *)rd->buf + pos, to_copy);

    debug("ramdisk_fetch: Read %lu bytes from pos %llu", to_copy, pos);

    return (long)to_copy;
}

static int ramdisk_cntl(struct storage *sto, int cmd, void *arg) {
    struct ramdisk *rd;

    trace("%s(%p, %d, %p)", __func__, sto, cmd, arg);

    if (sto == NULL) {
        return -EINVAL;
    }

    rd = (struct ramdisk *)sto;

    if (!rd->opened) {
        return -EINVAL;
    }

    if (cmd == FCNTL_GETEND) {
        if (arg == NULL) {
            return -EINVAL;
        }
        *(unsigned long long *)arg = rd->storage.capacity;
        debug("ramdisk_cntl: FCNTL_GETEND returns %llu", rd->storage.capacity);
        return 0;
    }
    
    if (cmd == FCNTL_MMAP) {
        kprintf("MMAP is not supported yet\n");
        return -ENOTSUP;
    }
    
    debug("ramdisk_cntl: Unsupported command %d", cmd);
    return -ENOTSUP;
}
