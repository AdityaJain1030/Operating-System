#ifndef _TESTSUITE_VIOBLK_H_
#define _TESTSUITE_VIOBLK_H_

#include "stdarg.h"

// Add more test prototypes here
// Add args if you want
void run_testsuite_vioblk(void);

int test_open_close(va_list ap);
int test_double_open(va_list ap);
int test_read(va_list ap);

#endif // _TESTSUITE_VIOBLK_H_