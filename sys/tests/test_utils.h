//tests and useful macros to make writing tests easier
#ifndef _TEST_UTILS_H_
#define _TEST_UTILS_H_

#include <stdarg.h>

#define DEVMNTNAME "dev"
#define CMNTNAME "c"
#define RAMDISK_NAME "ramdisk"
#define VIRTIOBLK_NAME "vioblk"
#define INSTNO 0

/**
 * Run a test, printing pass/fail, and other things
 * @param test_name: name to print
 * @param test: function to test, by convention function returns 0 on success, non-zero on failure
 * @param ...: forwarded to test via va_list
 */
int test_function(char* test_name, int (*test)(va_list), ...);


void print_buffer(void* buf, int size);

#endif /* _TEST_UTILS_H_ */
