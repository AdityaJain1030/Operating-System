//test suite from vioblk
#include "vioblk_test_suite.h"
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


//from the test_main file 
//
// #define CMNTNAME "c"
// #define DEVMNTNAME "dev"
// #define CDEVNAME "vioblk"
// #define CDEVINST 0

void run_vioblk_tests() {
    // int retval = -EINVAL;
    // char * test_output;
    //
    // retval = test_1();
    // test_output = (retval == 0) ? "test1 passed!" : "test1 failed!"; 
    //kprintf("I don't even think this function ran\n");

    test_vioblk_fetch_closed();
    test_vioblk_store_closed();
    test_vioblk_double_open();
    test_vioblk_open_close();
    test_vioblk_cntl_getend();


    //kprintf("if it ran then how did it not finish?\n");
    return;
}
int test_vioblk_open_close(){
    int retval = - EINVAL;
    struct storage * hd;
    hd = find_storage("vioblk",0);



    retval = storage_open(hd);
    if (retval != 0) {
        kprintf("storage_open failed on vioblk0: %s\n", error_name(retval));
        halt_failure();
    }
    storage_close(hd);


    kprintf("%s: first open+close was successful\n",__func__);


    retval = storage_open(hd);
    if (retval != 0) {
        kprintf("storage_open failed on vioblk0: %s\n", error_name(retval));
        halt_failure();
    }
    storage_close(hd);

    kprintf("%s: second open+close was successful\n",__func__);


    return 0;

}
// Make whatever tests you want.
int test_vioblk_double_open() {
    int retval = -EINVAL; 
    struct storage * hd;
    hd = find_storage("vioblk",0);

    retval =  storage_open(hd);
    if (retval != 0) {
        kprintf("storage_open failed on vioblk0: %s\n", error_name(retval));
        halt_failure();
    }

    retval =  storage_open(hd);
    if (retval != -EBUSY) {
        kprintf("driver opened up the same device twice...: %s\n", error_name(retval));

        storage_close(hd);
        halt_failure();
    }
    storage_close(hd);

    
    kprintf("%s: failed to open the device twice. correct behavior was achieved\n",__func__);

    return 0;
}

int test_vioblk_fetch_closed(){
    int retval = -EINVAL; 
    struct storage * hd;
    hd = find_storage("vioblk", 0);


    retval =  storage_open(hd);
    if (retval != 0) {
        kprintf("storage_open failed on vioblk0: %s\n", error_name(retval));
        halt_failure();
    }



    storage_close(hd);
    char buff[512];

    retval = storage_fetch(hd, 0, buff, 512);

    // for (size_t i = 0; i < 10; i++) {
    //     kprintf("%c", buff[i]);
    // }
    // kprintf("\n");

    if (!(retval < 0)){
        kprintf("%s: undesired behavior - storage fetch was suppoed to return EINVAL here because of attempt to store on closed. It didn't\n", __func__);
        halt_failure();
    }

    kprintf("%s: desired behavior reached: closed vioblk wasn't accessed\n", __func__);
    
    return 0;

}

int test_vioblk_store_closed(){
    int retval = -EINVAL; 
    struct storage * hd;
    hd = find_storage("vioblk", 0);


    retval =  storage_open(hd);
    if (retval != 0) {
        kprintf("storage_open failed on vioblk0: %s\n", error_name(retval));
        halt_failure();
    }



    storage_close(hd);
    char buff[512];

    retval = storage_store(hd, 0, buff, 512);

    if (!(retval < 0)){
        kprintf("%s: undesired behavior - storage store was suppoed to return EINVAL here because of attempt to store on closed. It didn't\n", __func__);
        halt_failure();
    }

    kprintf("%s: desired behavior reached: closed vioblk wasn't accessed\n", __func__);
    
    return 0;

}

int test_vioblk_cntl_getend(){
    int retval = -EINVAL;
    struct storage * hd;
    hd = find_storage("vioblk", 0);


    retval =  storage_open(hd);
    if (retval != 0) {
        kprintf("storage_open failed on vioblk0: %s\n", error_name(retval));
        halt_failure();
    }

    //hd.capacity is the number of bytes in the storage device
    unsigned long long end = 0;
    retval = storage_cntl(hd, FCNTL_GETEND, &end);
    if (retval != 0) {
        kprintf("storage_cntl failed on vioblk0: %s\n", error_name(retval));
        storage_close(hd);
        halt_failure();
    } 
    // if (hd->capacity != end){
    //     kprintf("%s: the end we found didn't match the capacity assigned to the storage object (%d)\n", __func__, end);
    //     storage_close(hd);
    //     halt_failure();
    // }
    kprintf("%s: the end we found is probably correct (%d)\n thats %d blocks btw.\n", __func__, end, end/512);
    
    storage_close(hd);
    return 0;

}
