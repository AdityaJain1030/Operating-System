#ifndef UMODE // cp1
    #include "uio.h"
    //#include "../../sys/console.h"
    void main(struct uio * uio) {
        uio_printf(uio, "Hello, world!\n");
    }
    // void main(void)
    // {
    //     int a = 1;
    //     int b = 2;
    //     int c  = a + b;
    //     kprintf("This is c: %d\n", c);

    // }
#endif

#ifdef UMODE // cp2&3
    #include "string.h"
    void main(void) {
        printf("Hello, world!\n");
    }
#endif