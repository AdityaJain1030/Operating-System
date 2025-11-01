#include "test_utils.h"
#include "error.h"
#include <stdarg.h>
#include "console.h"

int test_function(char* test_name, int (*test)(va_list), ...)
{
    int retval = -EINVAL;
    char * test_output;
    
    va_list ap;
    va_start(ap, test);
    retval = test(ap);
    va_end(ap);

    test_output = (retval == 0) ? "Passed!" : "Failed!";
    kprintf("%s: %s (ret: %d)\n", test_name, test_output, retval);
    return retval;
}

// example test to show yall how it works
int example_test(va_list ap)
{
    int val = va_arg(ap, int);
    char *s = va_arg(ap, char*);

    kprintf("example_test: val=%d s=%s\n", val, s);

    if (val > 0) return 0;
    return -EINVAL;
}