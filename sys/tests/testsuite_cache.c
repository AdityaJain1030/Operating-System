// #include <stdarg.h>
// #include <stdint.h>
// #include <string.h>
// #include "devimpl.h"
// #include "error.h"
// #include "console.h"
// #include "conf.h"
// #include "console.h"
// #include "device.h"
// #include "filesys.h"
// #include "fsimpl.h"
// #include "heap.h"
// #include "filesys.h"
// #include "ktfs.h"
// #include "cache.h"
// #include "see.h"
// #include "test_utils.h"
// #include "tests/filesystem_expected.h"
// #include "uio.h"

// #include "testsuite_filesystem.h"
// #include "filesystem_expected.h"

// // #define TEST_BLOB_SIZE 1459

// #define BACKEND RAMDISK_NAME

// void run_testsuite_cache() {
//     kprintf("---------------------KTFS TESTS---------------------\n\n");
//     if(test_function("mount", test_mount_ktfs)) halt_failure();
//     struct uio* file;


// }

// int test_load_very_large_file(va_list ap)
// {
//     char* filename = va_arg(ap, char*);
//     int size = va_arg(ap, int);
//     // char* expected = va_arg(ap, char*);
//     struct uio* file;

//     if (open_file(CMNTNAME, filename, &file) != 0) return -1;
    
//     char* contents = kmalloc(512*7);
//     long len;
//     for (int i = 0; i < size / (512 * 7); i++) {
//         len = uio_read(file, contents, 512*7);
//         // kprintf("\n%d", len);
//         if (len == 0)
//         {
//             kprintf("\n%i\n", i);
//             // return len;
//             break;
//         }
//         if (len < 0) {
//             uio_close(file);
//             return len;
//             break;
//         }
//     }

//     int out = 0;
//     contents[len] = '\n\0';
//     // if (len < 0) out = len;
//     // else out = strcmp(expected, contents);
    
//     kprintf(contents);
//     // kprintf("\n%d\n", strlen(contents));
//     // kprintf(expected);

//     uio_close(file);
//     return len;
// }
