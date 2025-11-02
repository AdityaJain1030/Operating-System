#include <stdarg.h>
#include <stdint.h>
#include "devimpl.h"
#include "error.h"
#include "console.h"
#include "conf.h"
#include "console.h"
#include "device.h"
#include "heap.h"
#include "dev/virtio.h"
#include "cache.h"
#include "test_utils.h"
#include "uio.h"

#include "testsuite_vioblk.h"

#define TEST_BLOB_SIZE 1459

void run_testsuite_vioblk() {
    kprintf("---------------------VIOBLK TESTS---------------------\n\n");
    struct storage* rd = find_storage(VIRTIOBLK_NAME, INSTNO);

   // open/close

    // if(test_function("open_ramdisk", test_open_ramdisk, rd)) return;
    // if(test_function("close_ramdisk", test_close_ramdisk, rd)) return;

    // if(test_function("open_after_close_ramdisk", test_open_ramdisk, rd)) return;

    // test_function("read_simple_ramdisk", test_ramdisk_read_simple, rd);  
    // test_function("read_oob_ramdisk", test_ramdisk_read_oob, rd);
    // test_function("read_oob_ramdisk2", test_ramdisk_read_oob2, rd);

    // test_function("cntl_ramdisk", test_cntl_ramdisk, rd);
    
    // if(test_function("close_ramdisk", test_close_ramdisk, rd)) return;
    
    // test_function("cntl_ramdisk_closed", test_cntl_ramdisk_closed, rd);
    // test_function("read_ramdisk_closed", test_read_ramdisk_closed, rd);

    // if(test_function("open_after_close_ramdisk", test_open_ramdisk, rd)) return;
    // if(test_function("close_ramdisk", test_close_ramdisk, rd)) return;

    
}

int test_open_close(va_list ap)
{
    struct storage* blk = va_arg(ap, struct storage*);
    int result = storage_open(blk);

    if (result != 0) {
        kprintf("open failed");
        return result;
    }

    storage_close(blk);

    result = storage_open(blk);

    if (result == -EBUSY) {
        kprintf("closed failed");
        return result;
    }

    if (result != 0) {
        kprintf("open after close failed");
        return result;
    }

    storage_close(blk);

    return 0;
}

int test_double_open(va_list ap)
{
    struct storage* blk = va_arg(ap, struct storage*);
    int result = storage_open(blk);
    result = storage_open(blk);

    if (result == 0) result = 1; // should not open
    if (result != -EBUSY) return result;

    storage_close(blk);
    return 0;
}

int test_read(va_list ap)
{
    struct storage* blk = va_arg(ap, struct storage*);
    unsigned long long pos = va_arg(ap, unsigned long long);
    unsigned long bufsz = va_arg(ap, unsigned long);
    int expected_size = va_arg(ap, int);
    char* expected_data = va_arg(ap, char*);


    storage_open(blk);
    char* buf = kmalloc(bufsz);
    int result = storage_fetch(blk, pos, blk, bufsz);

    if (expected_size < 0) return (result >= 0);

}