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

#define TEST_BLOB_SIZE 512

void run_testsuite_vioblk() {
    kprintf("---------------------VIOBLK TESTS---------------------\n\n");
    struct storage* blk = find_storage(VIRTIOBLK_NAME, INSTNO);

    // simple open tests
    if(test_function("open_close_vioblk", test_open_close, blk)) return;
    if(test_function("double_open_vioblk", test_double_open, blk)) return;

    // read tests
    test_function("read_vioblk_within_bounds", test_read, blk, 0, TEST_BLOB_SIZE, (unsigned int)-1, NULL);
    test_function("read_vioblk_oob_pos", test_read, blk, (unsigned long long)-1, 20, -1, NULL);
    test_function("read_vioblk_unaligned_size", test_read, blk, 0, TEST_BLOB_SIZE + 1, 512, NULL);
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
    int result = storage_fetch(blk, pos, buf, bufsz);

    if (expected_size == (unsigned int)-1) return (result < 0);

    if (result != expected_size) {
        kprintf("read size mismatch: expected %d, got %d\n", expected_size, result);
        kfree(buf);
        storage_close(blk);
        return result;
    }

    if (expected_data == NULL) return 0;

    if (strcmp(buf, expected_data) != 0) {
        kprintf("read data mismatch\n");
        kfree(buf);
        storage_close(blk);
        return strcmp(buf, expected_data);
    }

}