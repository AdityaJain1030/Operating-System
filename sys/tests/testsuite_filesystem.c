#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "devimpl.h"
#include "error.h"
#include "console.h"
#include "conf.h"
#include "console.h"
#include "device.h"
#include "filesys.h"
#include "fsimpl.h"
#include "heap.h"
#include "filesys.h"
#include "ktfs.h"
#include "cache.h"
#include "see.h"
#include "test_utils.h"
#include "tests/filesystem_expected.h"
#include "uio.h"

#include "testsuite_filesystem.h"
#include "filesystem_expected.h"

#define TEST_BLOB_SIZE 1459

#define BACKEND RAMDISK_NAME

void run_testsuite_filesystem() {
    kprintf("---------------------KTFS TESTS---------------------\n\n");
    if(test_function("mount", test_mount_ktfs)) halt_failure();
    struct uio** file;
    // if(test_function("open_file", test_open_file, abc_short_txt_name, file)) return;
    // if(test_function("close_file", test_close_file, file)) return;

    test_function("read_file_contents_simple", test_read_file_contents, abc_short_txt_name, abcs_short_txt_content);
    // test_function("read_file_contents_long", test_read_file_contents, count_long_txt_name, count_long_txt_content);
}


int test_mount_ktfs(va_list ap) {
    struct storage * hd;
    struct cache * cache;
    int result;

    hd = find_storage(BACKEND, INSTNO);

    if (hd == NULL) {
        kprintf("Storage device %s%d not found\n", BACKEND, INSTNO);
        return -1;
    }

    result = storage_open(hd);

    if (result != 0) {
        kprintf("storage_open failed on %s%d: %s\n",
            BACKEND, INSTNO, error_name(result));
        return -1;
        // halt_failure();
    }

    result = create_cache(hd, &cache);

    if (result != 0) {
        kprintf("create_cache(%s%d) failed: %s\n",
            BACKEND, INSTNO, error_name(result));
        // halt_failure();
        return -1;
    }

    result = mount_ktfs(CMNTNAME, cache);

    if (result != 0) {
        kprintf("mount_ktfs(%s, cache(%s%d)) failed: %s\n",
            CMNTNAME, BACKEND, INSTNO, error_name(result));
        // halt_failure();
        return -1;
    }
    return 0;
}

int test_open_file(va_list ap)
{
    char* filename = va_arg(ap, char*);
    struct uio** file_loc = va_arg(ap, struct uio**);

    return open_file(CMNTNAME, filename, file_loc);
}

int test_close_file(va_list ap)
{
    struct uio* file = va_arg(ap, struct uio*);
    // need a better way to test
    uio_close(file);
    return 0;
}

int test_read_file_contents(va_list ap)
{
    char* filename = va_arg(ap, char*);
    char* expected = va_arg(ap, char*);
    struct uio* file;

    if (!open_file(CMNTNAME, filename, &file)) return -1;
    
    char* contents = kmalloc(sizeof(expected));
    long len = uio_read(file, contents, sizeof(expected));

    int out = 0;
    kprintf(contents);
    if (len < 0) out = len;
    else out = strcmp(expected, contents);

    uio_close(file);
    return out;
}