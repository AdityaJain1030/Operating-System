#include <stdarg.h>
#include <stdint.h>
#include "devimpl.h"
#include "error.h"
#include "console.h"
#include "conf.h"
#include "console.h"
#include "device.h"
#include "heap.h"
#include "dev/ramdisk.h"
#include "cache.h"
#include "test_utils.h"
#include "uio.h"

#include "testsuite_ramdisk.h"

#define TEST_BLOB_SIZE 1459

void run_testsuite_ramdisk() {
    if(test_function("attach_ramdisk", test_attach_ramdisk)) return;

    struct storage* rd = find_storage(RAMDISK_NAME, INSTNO);

    if(test_function("open_ramdisk", test_open_ramdisk, rd)) return;
    if(test_function("close_ramdisk", test_close_ramdisk, rd)) return;

    if(test_function("open_after_close_ramdisk", test_open_ramdisk, rd)) return;

    test_function("read_simple_ramdisk", test_ramdisk_read_simple, rd);  
    test_function("read_oob_ramdisk", test_ramdisk_read_oob, rd);
    test_function("read_oob_ramdisk2", test_ramdisk_read_oob2, rd);

    test_function("cntl_ramdisk", test_cntl_ramdisk, rd);
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

int test_cntl_ramdisk(va_list ap)
{
    struct storage* rd = va_arg(ap, struct storage*);
    unsigned long long* buf = kmalloc(sizeof(unsigned long long));
    long len = storage_cntl(rd, FCNTL_GETEND, buf);
    // kprintf("%d \n", *buf);
    if (len != 0) return (int) len;
    if (*buf != TEST_BLOB_SIZE) {
        kprintf("bad buffer size");
        int x = *buf;
        kfree(buf);
        return x;
    }
    return 0;
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
    uint8_t* buf = kmalloc(520);
    long len = storage_fetch(rd, 0,  buf, 520);
    // kprintf("%d \n", len);
    kfree(buf);
    if (len < 0) return (int) len;
    return 0;
}

int test_ramdisk_read_oob(va_list ap)
{
    struct storage* rd = va_arg(ap, struct storage*);
    uint8_t* buf = kmalloc(520);
    long len = storage_fetch(rd, 1000,  buf, 520);
    // kprintf("%d \n", len);
    kfree(buf);
    if (len != -EINVAL) return (int) len;
    return 0;
}

int test_ramdisk_read_oob2(va_list ap)
{
    struct storage* rd = va_arg(ap, struct storage*);
    uint8_t* buf = kmalloc(1024);
    long len = storage_fetch(rd, 512,  buf, 1024);
    // kprintf("%d \n", len);
    kfree(buf);
    if (len != TEST_BLOB_SIZE - 512) return (int) len;
    return 0;
}