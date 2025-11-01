#ifndef _TESTSUITE_RAMDISK_H_
#define _TESTSUITE_RAMDISK_H_

#include "stdarg.h"

// Add more test prototypes here
// Add args if you want
void run_testsuite_ramdisk(void);

int test_attach_ramdisk(va_list ap);
int test_open_ramdisk(va_list ap);
int test_ramdisk_read_simple(va_list ap);
int test_ramdisk_read_oob(va_list ap);
int test_ramdisk_read_oob2(va_list ap);
int test_close_ramdisk(va_list ap);
int test_cntl_ramdisk(va_list ap);

#endif // _TESTSUITE_RAMDISK_H_