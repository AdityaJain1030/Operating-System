/*! @file main.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌‍‌⁠​⁠⁠‌
    @brief main function of the kernel (called from start.s)
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "cache.h"
#include "conf.h"
#include "console.h"
#include "dev/ramdisk.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "device.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "string.h"
#include "thread.h"
#include "timer.h"

#define INITEXE "trek"  // FIXME

#define CMNTNAME "c"
#define DEVMNTNAME "dev"
#define CDEVNAME "ramdisk"
#define CDEVINST 0

#ifndef NUART  // number of UARTs
#define NUART 2
#endif

#ifndef NVIODEV  // number of VirtIO devices
#define NVIODEV 8
#endif

static void attach_devices(void);
static void mount_cdrive(void);  // mount primary storage device ("C drive")
static void run_init(void);

void main(void) {
    extern char _kimg_end[];  // provided by kernel.ld
    console_init();
    intrmgr_init();
    devmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, RAM_END);

    attach_devices();

    enable_interrupts();
    ramdisk_attach();

    mount_cdrive();
    run_init();
}

void attach_devices(void) {
    int i;
    int result;

    rtc_attach((void*)RTC_MMIO_BASE);

    for (i = 0; i < NUART; i++) attach_uart((void*)UART_MMIO_BASE(i), UART0_INTR_SRCNO + i);

    for (i = 0; i < NVIODEV; i++) attach_virtio((void*)VIRTIO_MMIO_BASE(i), VIRTIO0_INTR_SRCNO + i);

    result = mount_devfs(DEVMNTNAME);

    if (result != 0) {
        kprintf("mount_devfs(%s) failed: %s\n", CDEVNAME, error_name(result));
        halt_failure();
    }
}

void mount_cdrive(void) {
    struct storage* hd;
    struct cache* cache;
    int result;

    hd = find_storage(CDEVNAME, CDEVINST);

    if (hd == NULL) {
        kprintf("Storage device %s%d not found\n", CDEVNAME, CDEVINST);
        halt_failure();
    }

    result = storage_open(hd);

    if (result != 0) {
        kprintf("storage_open failed on %s%d: %s\n", CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = create_cache(hd, &cache);

    if (result != 0) {
        kprintf("create_cache(%s%d) failed: %s\n", CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = mount_ktfs(CMNTNAME, cache);

    if (result != 0) {
        kprintf("mount_ktfs(%s, cache(%s%d)) failed: %s\n", CMNTNAME, CDEVNAME, CDEVINST,
                error_name(result));
        halt_failure();
    }
}

void run_init(void) {
    struct uio* initexe;
    struct uio* uart_dev;
    int result;

    result = open_file(CMNTNAME, INITEXE, &initexe);

    if (result != 0) {
        kprintf(INITEXE ": %s; terminating\n", error_name(result));
        halt_failure();
    }

    result = open_file(DEVMNTNAME, "uart1", &uart_dev);
    
    if (result != 0) {
        kprintf(INITEXE ": %s; terminating\n", error_name(result));
        halt_failure();
    }
    // FIXME
    //  Run your executable here
    //  Note that trek takes in a uio object to output to the console
    // void (*start_trek)(struct uio*);
    void (*start_adele)(struct uio*);

    elf_load(initexe, &start_adele);

    start_adele(uart_dev);
}