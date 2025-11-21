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
#include "dev/ramdisk.h"
#include "device.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "process.h"
#include "string.h"
#include "thread.h"
#include "timer.h"

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
    memory_init();
    thrmgr_init();
    procmgr_init();
    // heap_init(_kimg_end, RAM_END);

    attach_devices();

    enable_interrupts();
    ramdisk_attach();

    // struct uio *trek;
    // int err = open_file(const char *mpname, const char *flname, struct uio **uioptr)
    mount_cdrive();
    run_init();

}

void attach_devices(void) {
    int i;
    int result;

    rtc_attach((void*)RTC_MMIO_BASE);

    for (i = 0; i < NUART; i++) attach_uart((void*)UART_MMIO_BASE(i), UART0_INTR_SRCNO + i);

    for (i = 0; i < NVIODEV; i++) attach_virtio((void*)VIRTIO_MMIO_BASE(i), VIRTIO0_INTR_SRCNO + i);

    // attach ramdisk
    //ramdisk_attach();

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


int test_uio_control_ramdisk_read();
int test_elf_load_with_ramdisk_uio();
void run_init(void) {
    struct uio* initexe;
    struct uio* uart_dev;
    struct uio* ramdisk_dev;
    int result;

    result = open_file(CMNTNAME, INITEXE, &initexe);

    if (result != 0) {
        kprintf(INITEXE ": %s; terminating\n", error_name(result));
        halt_failure();
    }

    // result = open_file(DEVMNTNAME, "uart1", &uart_dev);
    // //result = open_file(DEVMNTNAME, "ramdisk0", &ramdisk_dev);
    // if (result != 0) {
    //     kprintf("UART1 : %s; terminating\n", error_name(result));
    //     halt_failure();
    // }
    // FIXME
    //  Run your executable here
    //  Note that trek takes in a uio object to output to the console
    // void (*start_trek)(struct uio*);
    // void (*start_adele)(struct uio*);
    struct process *curr = running_thread_process();
    open_file(DEVMNTNAME, "uart1",&curr->uiotab[2]);
    char *argv[] = { "trek", NULL };
    process_exec(initexe, 1, argv);
    // elf_load(initexe, &start_trek);
    // start_adele(uart_dev);

    // stuff I added
    //test_uio_control_ramdisk_read();
    // test_elf_load_with_ramdisk_uio();
}



int test_uio_control_ramdisk_read() {
    struct uio* ruio;
    int retval;
    kprintf("TEST_UIO_RAMDISK_READ!\n");
    ramdisk_attach();
    open_file(DEVMNTNAME, "ramdisk0", &ruio);
    char buf[50];
    unsigned long long pos = 5;
    retval = uio_cntl(ruio, FCNTL_SETPOS, &pos);
    if (retval != 0) {
        kprintf("Failed to set pos of ramdisk\n");
        return -1;
    }
    unsigned long long disksz;
    retval = uio_cntl(ruio, FCNTL_GETEND, &disksz);
    if (retval != 0) {
        kprintf("Failed to get end of disk\n");
        return -1;
    }
    kprintf("disksz = %u\n", disksz);
    retval = uio_read(ruio, buf, 10);
    for (int i = 0; i < 10; ++i) {
        kprintf("buf[%d] = %x\n", i, buf[i]);
    }

    retval = uio_cntl(ruio, FCNTL_GETPOS, &pos);
    if (retval != 0) {
        kprintf("Failed to get position of ramdisk\n");
        return -1;
    }
    kprintf("Position of ramdisk uio is %u\n", pos);
    return 0;
}
struct uio {
    const struct uio_intf *intf;
    int refcnt;
};

int test_elf_load_with_ramdisk_uio() {
    struct uio* ruio;
    struct uio* termio;
    int retval;
    kprintf("TEST_ELF_LOAD!\n");
    ramdisk_attach();
    open_file(DEVMNTNAME, "ramdisk0", &ruio);
    int result = open_file(DEVMNTNAME, "uart1", &termio);
    if (result != 0) {
        kprintf(" ELF LOAD UART OPENING FIALED!: %s; terminating\n", error_name(result));
        halt_failure();
    }
    kprintf("termoo: %d\n", termio->refcnt);
    void (*entry_ptr)(struct uio*);
    retval = elf_load(ruio,(void (**)(void)) &entry_ptr);
    if (retval < 0) {
        kprintf("elf load failed with retval %d\n", retval);
        return -1;
    }
    kprintf("Spawning the Threaed now!\n");
    retval = spawn_thread("hellothr", (void (*)(void)) entry_ptr, termio);
    if (retval < 0) {
        kprintf("spawn thread failed with retval %d\n", retval);
        return -1;
    }
    thread_join(retval);
    return 0;
}