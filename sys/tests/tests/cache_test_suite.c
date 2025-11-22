#include "cache_test_suite.h"
// #include "error.h"
// #include "misc.h"
// #include "console.h"
// #include "dev/virtio.h"
// #include "device.h"
// #include <string.h>
// #include "cache.h"
// //#include "cachetests.h"
// #include "conf.h"
// #include "console.h"
// #include "device.h"
// #include "devimpl.h"
// #include "error.h"
// #include "heap.h"
// #include "memory.h"
// #include "misc.h"
// #include "string.h"
// #include "thread.h" 
// #include "timer.h"
#include "conf.h"
#include "console.h"
#include "intr.h"
#include "device.h"
#include "thread.h"
#include "heap.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "timer.h"
#include "misc.h"
#include "string.h"
#include "filesys.h"
#include "error.h"
#include "cache.h"



// Add args, structs, includes, defines
void run_cache_tests() {
    // int retval = -EINVAL;
    // char * test_output;

    // retval = test_1();
    // test_output = (retval == 0) ? "test1 passed!" : "test1 failed!"; 
    // kprintf("%s\n", test_output);
    observe_cache_lru();
}

//this test has no output. its jjust to observe the flow
int observe_cache_lru() {
    
    struct uio * bee_uio;
    struct uio * lorem_uio;
    open_file("c","bee_movie.txt", &bee_uio);
    open_file("c","lorem.txt", &lorem_uio);

    char buff1[512];
    char buff2[512];
    
    
    for (int i = 0; i < 5000; i++){
        if (i%2 ==0) uio_read(bee_uio, buff1, (i * 5)%43);
        if (i%3 ==0) uio_read(bee_uio, buff1, (i * 7)%47);
    }

    uio_close(bee_uio);
    uio_close(lorem_uio);
    
    return 0;
}


int test_lru_concurrency(){
    int retval = -EINVAL;
     
    struct uio * bee_uio;
    open_file("c","bee_movie.txt", &bee_uio);

    char buff1[512];

    for (int i =0; i < 64; i++){
        uio_read(bee_uio, buff1, 512);
        
    }// added a condition wait in ktfs such that 


    //at this point lru should be block 0. so 
    
}
