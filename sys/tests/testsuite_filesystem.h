#ifndef _TESTSUITE_FILESYSTEM_H_
#define _TESTSUITE_FILESYSTEM_H_

#include "stdarg.h"

// Add more test prototypes here
// Add args if you want
void run_testsuite_filesystem(void);

int test_mount_ktfs(va_list ap);
int test_open_file(va_list ap);
int test_close_file(va_list ap);
int test_read_file_contents(va_list ap);
int test_load_very_large_file(va_list ap);

#endif // _TESTSUITE_FILESYSTEM_H_