// test_main.c - test main function of the kernel (called from start.s)
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "conf.h"
#include "console.h"
#include "intr.h"
#include "device.h"
#include "thread.h"
#include "heap.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "timer.h"
#include "string.h"
#include "dev/ramdisk.h"
#include "filesys.h"
#include "error.h"
#include "cache.h"

// include testsuite(s)
#include "testsuite_1.h"
#include "testsuite_ramdisk.h"

#ifndef CMNTNAME
#define CMNTNAME "c"
#endif
#ifndef DEVMNTNAME
#define DEVMNTNAME "dev"
#endif
#ifndef CDEVNAME
#define CDEVNAME "vioblk"
#endif
#ifndef CDEVINST
#define CDEVINST 0
#endif

#ifndef NUART // number of UARTs
#define NUART 2
#endif

#ifndef NVIODEV // number of VirtIO devices
#define NVIODEV 8
#endif

static void attach_devices(void);
static void mount_cdrive(void); // mount primary storage device ("C drive")
static void mount_cdrive_ramdisk(void); // mount primary storage device ("C drive")


void main(void) {
    extern char _kimg_end[]; // provided by kernel.ld
    console_init();
    intrmgr_init();
    devmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, RAM_END);

    attach_devices();

    enable_interrupts();

    run_testsuite_ramdisk();
    // mount_cdrive_ramdisk();

    // Run the testsuite
    // run_testsuite_1();
}

void attach_devices(void) {
    int i;
    int result;

    rtc_attach((void*)RTC_MMIO_BASE);

    for (i = 0; i < NUART; i++)
        attach_uart((void*)UART_MMIO_BASE(i), UART0_INTR_SRCNO+i);
    
    for (i = 0; i < NVIODEV; i++)
        attach_virtio((void*)VIRTIO_MMIO_BASE(i), VIRTIO0_INTR_SRCNO+i);

    result = mount_devfs(DEVMNTNAME);

    if (result != 0) {
        kprintf("mount_devfs(%s) failed: %s\n",
            CDEVNAME, error_name(result));
        halt_failure();
    }
}


void mount_cdrive_ramdisk(void) {
    struct storage * hd;
    struct cache * cache;
    int result;

    ramdisk_attach();
    
    hd = find_storage("ramdisk", CDEVINST);

    if (hd == NULL) {
        kprintf("Storage device %s%d not found\n", "ramdisk", CDEVINST);
        halt_failure();
    }

    result = storage_open(hd);

    if (result != 0) {
        kprintf("storage_open failed on %s%d: %s\n",
            "ramdisk", CDEVINST, error_name(result));
        halt_failure();
    }

    result = create_cache(hd, &cache);

    if (result != 0) {
        kprintf("create_cache(%s%d) failed: %s\n",
            "ramdisk", CDEVINST, error_name(result));
        halt_failure();
    }

    result = mount_ktfs("ramdisk", cache);

    if (result != 0) {
        kprintf("mount_ktfs(%s, cache(%s%d)) failed: %s\n",
            "ramdisk", CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }
}

void mount_cdrive(void) {
    struct storage * hd;
    struct cache * cache;
    int result;

    hd = find_storage(CDEVNAME, CDEVINST);

    if (hd == NULL) {
        kprintf("Storage device %s%d not found\n", CDEVNAME, CDEVINST);
        halt_failure();
    }

    result = storage_open(hd);

    if (result != 0) {
        kprintf("storage_open failed on %s%d: %s\n",
            CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = create_cache(hd, &cache);

    if (result != 0) {
        kprintf("create_cache(%s%d) failed: %s\n",
            CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = mount_ktfs(CMNTNAME, cache);

    if (result != 0) {
        kprintf("mount_ktfs(%s, cache(%s%d)) failed: %s\n",
            CMNTNAME, CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }
}