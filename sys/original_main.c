/* 
 @brief main function of the kernel (called from start.s)
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "cache.h"
#include "conf.h"
#include "console.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "dev/ramdisk.h"

#define INITEXE "trek"  // FIXME

#define CMNTNAME "c"
#define DEVMNTNAME "dev"
#define CDEVNAME "vioblk"
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

    mount_cdrive();
    run_init();
}

void attach_devices(void) {
    int i;
    int result;

    rtc_attach((void*)RTC_MMIO_BASE);

    for (i = 0; i < NUART; i++) attach_uart((void*)UART_MMIO_BASE(i), UART0_INTR_SRCNO + i);

    for (i = 0; i < NVIODEV; i++) attach_virtio((void*)VIRTIO_MMIO_BASE(i), VIRTIO0_INTR_SRCNO + i);
    ramdisk_attach();
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
    int result;

    // result = open_file(CMNTNAME, INITEXE, &initexe);

    // if (result != 0) {
    //     kprintf(INITEXE ": %s; terminating\n", error_name(result));
    //     halt_failure();
    // }

    // FIXME
    //  Run your executable here
    //  Note that trek takes in a uio object to output to the console

    // loading ramdisk
    struct storage* str = find_storage(RAMDISK_NAME, INSTNO);
    ramdisk_make_uio((struct ramdisk*) str, initexe);
    kprintf("DONE!\n");

}
// void run_init(void) {
//     struct uio * trek_term;
//     struct uio * seedsrc;
//     unsigned long rngseed = 0xECE391;

//     if(open_file(DEVMNTNAME, "uart1", &trek_term) != 0 || trek_term == NULL) {
//         kprintf("mount_ktfs(%s%d) failed: %s\n", CDEVNAME, CDEVINST);
//         halt_failure();
//     }
//     if (open_file(DEVMNTNAME, "viorng0", &seedsrc) == 0 && seedsrc != NULL) {
//         (void)uio_read(seedsrc, &rngseed, sizeof(rngseed));
//         uio_close(seedsrc);
//         seedsrc = NULL;
//     }

//     struct uio* initexe;
//     int result;
//     user_entry_t entry = NULL;
//     result = open_file(CMNTNAME, INITEXE, &initexe);

//     if (result != 0) {
//         kprintf(INITEXE ": %s; terminating\n", error_name(result));
//         halt_failure();
//     }
//     int rv = elf_load(initexe, (void*)&entry);
//     if(rv != 0) {
//         kprintf(INITEXE ": %s; terminating\n", error_name(rv));
//         halt_failure();
//     }
//     fsmgr_flushall();
//     uint32_t *w = (uint32_t*)(uintptr_t)entry;
//     kprintf("entry 0x%px\n",entry);
//     kprintf("entry bytes: %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3]);
//     entry(trek_term);
//     uio_close(trek_term);

//     // FIXME
//     //  Run your executable here
//     //  Note that trek takes in a uio object to output to the console
// }