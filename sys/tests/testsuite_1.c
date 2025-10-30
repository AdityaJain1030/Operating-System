#include "testsuite_1.h"
#include "error.h"
#include "console.h"
#include "conf.h"
#include "console.h"
#include "intr.h"
#include "device.h"
#include "thread.h"
#include "heap.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "dev/ramdisk.h"
#include "uio.h"
#include "cache.h"
#include "elf.h"
#include "filesys.h"

#define DEVMNTNAME "dev"


// Add args, structs, includes, defines
void run_testsuite_1() {
    int retval = -EINVAL;
    char * test_output;

    retval = test_1();
    test_output = (retval == 0) ? "test1 passed!" : "test1 failed!"; 
    kprintf("%s\n", test_output);

    retval = test_find_storage();
    test_output = (retval == 0) ? "test_find_storage passed!" : "test_find_storage failed!"; 
    kprintf("%s\n", test_output);

    retval = test_simple_storage_read();
    test_output = (retval == 0) ? "test_simple_storage_read passed!" : "test_simple_storage_read failed!"; 
    kprintf("%s\n", test_output);

    retval = test_simple_storage_write();
    test_output = (retval == 0) ? "test_simple_storage_write passed!" : "test_simple_storage_write failed!"; 
    kprintf("%s\n", test_output);

    retval = test_simple_ramdisk_uio_read();
    test_output = (retval == 0) ? "test_simple_ramdisk_uio_read passed!" : "test_simple_ramdisk_uio_read failed!"; 
    kprintf("%s\n", test_output);

    retval = test_uio_control_ramdisk_read();
    test_output = (retval == 0) ? "test_uio_control_ramdisk_read passed!" : "test_uio_control_ramdisk_read failed!"; 
    kprintf("%s\n", test_output);

    retval = test_cache_get_and_release_block();
    test_output = (retval == 0) ? "test_cache_get_and_release_block passed!" : "test_cache_get_and_release_block failed!"; 
    kprintf("%s\n", test_output);

}

// Make whatever tests you want.
int test_1() {
    return 0;
}

int test_find_storage() {
    struct storage* hd;
    hd = find_storage("vioblk", 0);
    if (hd == NULL) {
        kprintf("Storage device not found\n");
        return -1;
    } else {
        kprintf("Storage device found\n");
    }
    return 0;
}

int test_simple_storage_read() {
    struct storage* hd;
    int retval;
    hd = find_storage("vioblk", 0);
    if (hd == NULL) {
        kprintf("Storage device not found\n");
        return -1;
    } else {
        kprintf("Storage device found\n");
    }

    retval = storage_open(hd);
    if (retval != 0) {
        kprintf("failed to open storage, error code %d \n", retval);
        return -1;
    } else {
        kprintf("opened storage\n");
    }

    char buf[512];

    retval = storage_fetch(hd, 0, buf, 512);
    if (retval != 512) {
        kprintf("failed to fetch from storage\n");
        return -1;
    } else {
        kprintf("fetched from storage %d bytes\n", retval);
    }
    for (int i = 0; i < 512; ++i) {
        kprintf("buf[%d] = %x\n", i, buf[i]);
    }
    return 0;
}

int test_simple_storage_write() {
    struct storage* hd;
    int retval;
    hd = find_storage("vioblk", 0);
    if (hd == NULL) {
        kprintf("Storage device not found\n");
        return -1;
    } else {
        kprintf("Storage device found\n");
    }

    retval = storage_open(hd);
    if (retval != 0) {
        kprintf("failed to open storage\n");
        return -1;
    } else {
        kprintf("opened storage\n");
    }

    char wdata[512];
    for (int i = 0; i < 512; ++i) {
        wdata[i] = 511 - i;
    }

    retval = storage_store(hd, 0, wdata, 512);
    if (retval != 512 ) {
        kprintf("failed to write to storage\n");
        return -1;
    }

    char rdata[512];

    retval = storage_fetch(hd, 0, rdata, 512);
    if (retval != 512) {
        kprintf("failed to fetch from storage\n");
    } else {
        kprintf("fetched from storage %d bytes\n", retval);
    }
    for (int i = 0; i < 512; ++i) {
        kprintf("rdata[%d] = %d\n", i, rdata[i]);
    }
    return 0;
}

int test_simple_ramdisk_uio_read() {
    struct uio* ruio;
    ramdisk_attach();
    open_file(DEVMNTNAME, "ramdisk0", &ruio);
    char buf[50];
    int retval = uio_read(ruio, buf, 50);
    if (retval != 50) {
        return -1;
    }
    for (int i = 0; i < 50; ++i) {
        kprintf("buf[%d] = %x\n", i, buf[i]);
    }
    return 0;
}

int test_uio_control_ramdisk_read() {
    struct uio* ruio;
    int retval;
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

// int test_elf_load_with_ramdisk_uio() {
//     struct uio* ruio;
//     struct uio* termio;
//     int retval;
//     ramdisk_attach();
//     open_file(DEVMNTNAME, "ramdisk0", &ruio);
//     open_file(DEVMNTNAME, "uart1", &termio);
//     void (*entry_ptr)(struct uio*);
//     retval = elf_load(ruio, &entry_ptr);
//     if (retval < 0) {
//         kprintf("elf load failed with retval %d\n", retval);
//         return -1;
//     }
//     retval = spawn_thread("hellothr", entry_ptr, termio);
//     if (retval < 0) {
//         kprintf("spawn thread failed with retval %d\n", retval);
//         return -1;
//     }
//     thread_join(retval);
//     return 0;
// }

int test_cache_get_and_release_block() {
    struct storage* disk;
    struct cache* cptr;
    ramdisk_attach();
    disk = find_storage("ramdisk", 0);
    storage_open(disk);
    create_cache(disk, &cptr);
    char* buf;
    cache_get_block(cptr, 0, (void**)&buf);
    for (int i = 0; i < 512; ++i) {
        kprintf("buf[%d] = %x\n", i, buf[i]);
    }
    cache_release_block(cptr, buf, 0);
}