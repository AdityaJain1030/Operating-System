#include <stdarg.h>
#include "devimpl.h"
#include "error.h"
#include "console.h"
#include "conf.h"
#include "console.h"
#include "intr.h"
#include "device.h"
#include "thread.h"
#include "heap.h"
#include "dev/ramdisk.h"
#include "uio.h"
#include "cache.h"
#include "elf.h"
#include "filesys.h"
#include "test_utils.h"

#include "testsuite_ramdisk.h"

void run_testsuite_ramdisk() {
    if(test_function("attach_ramdisk", test_attach_ramdisk)) return;

    struct storage* rd = find_storage(RAMDISK_NAME, INSTNO);

    if(test_function("open_ramdisk", test_open_ramdisk, rd)) return;
    test_function("read_simple_ramdisk", test_ramdisk_read_simple, rd);  
    if(test_function("open_ramdisk", test_close_ramdisk, rd)) return;
}

int test_attach_ramdisk(va_list ap)
{
    ramdisk_attach();
    struct storage* rd = find_storage(RAMDISK_NAME, INSTNO);

    if (rd != NULL) return 0;
    return -ENOENT; // closest error I can find
}

int test_open_ramdisk(va_list ap)
{
    struct storage* rd = va_arg(ap, struct storage*);
    return storage_open(rd);
}

int test_close_ramdisk(va_list ap)
{
    struct storage* rd = va_arg(ap, struct storage*);
    storage_close(rd);
    // we check if closed properly by trying to open again
    int check = !storage_open(rd);
    if (!check) return check;
    storage_close(rd);
    return 0;
}

int test_ramdisk_read_simple(va_list ap)
{
    struct storage* rd = va_arg(ap, struct storage*);
    uint8_t* buf = kmalloc(512);
    long len = storage_fetch(rd, 0,  buf, 512);
    kfree(buf);
    if (len < 0) return (int) len;
    return 0;
}