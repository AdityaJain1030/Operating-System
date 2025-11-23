//test suite from ktfs_highlevel_test_suite
#include "ktfs_highlevel_test_suite.h"
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
#include <stdint.h>

//from the test_main file 
//
// #define CMNTNAME "c"
// #define DEVMNTNAME "dev"
// #define CDEVNAME "vioblk"
// #define CDEVINST 0

#define LOREM_BYTE_LEN 273284
#define BEE_MOVIE_BYTE_LEN 148423

static char buff1[BEE_MOVIE_BYTE_LEN];
static char buff2[BEE_MOVIE_BYTE_LEN];
static char buff3[BEE_MOVIE_BYTE_LEN];

void dump_buffer(const char *buf, size_t len) { if (!buf) return;
    for (size_t i = 0; i < len; i++) {
        kprintf("%c", buf[i]);
    }
    kprintf("\n");
}


void run_ktfs_highlevel_tests() {
    //test_ktfs_multiopen();    
    //test_ktfs_multiclose(); //unused

    //test_ktfs_unfoundopen(); 
    test_ktfs_fetch_dindirection();
    //kprintf("reached\n");
    //test_ktfs_getpos_through_fetch();
    test_ktfs_setpos_through_fetch();
    test_ktfs_fetch_dindirection();

    //kprintf("reached\n");
    //test_ktfs_getend();
    //kprintf("reached\n");
    
    //test_ktfs_multiple_uio_reference();
    //test_ktfs_setpos_past_end();

    //kprintf("reached\n");
    //test_ktfs_open_many(); //ALWAYS MAKE SURE THAT THIS IS LAST BECAUSE I DON"T EVER CLOSE THE UIOS
    return;
}

// Make whatever tests you want.
int test_ktfs_multiopen() {
    int retval = -EINVAL;
    

    //kprintf("reached\n");
    struct uio * uio;
    retval = open_file("c","bee_movie.txt",&uio);
    if (retval <0) {
        kprintf("%s: ktfs failed to open the first file\n", __func__);
        return retval; 
    }else kprintf("%s: ktfs opened the first file successfully\n", __func__);

    
    //kprintf("reached\n");
    retval = open_file("c","bee_movie.txt",&uio);
    if (retval ==0) { 
    kprintf("%s: unintended behavior. ktfs opened the same file twice\n", __func__);
    halt_failure();
    } else kprintf("%s: ktfs failed to open the same file twice, as is intended\n", __func__);
    
    //opens up a different file to make sure that open/close is an independent event for different files 
    uio_close(uio);

    //kprintf("reached\n");

    retval = open_file("c","lorem.txt",&uio);
    if (retval <0) {
        kprintf("%s: ktfs failed to open the second file\n", __func__);
        return retval; 
    }else kprintf("%s: ktfs opened the second file successfully\n", __func__);

    struct uio * second_uio;
    retval = open_file("c","bee_movie.txt",&second_uio);
    if (retval <0) { 
    kprintf("%s: unintended behavior. the opening of bee_movie shouldn't be affected by the opening of lorem\n", __func__);
    uio_close(uio);
    return retval;
    } else kprintf("%s: opened the second file successfully, as is intended\n", __func__);

    uio_close(uio);
    uio_close(second_uio);

    return 0;
}

//opening a file that shouldn't be in the filesystem (yet)
int test_ktfs_unfoundopen() {
    int retval = -EINVAL;
    
    struct uio * uio;
    retval = open_file("c","unknown.404",&uio); //NOTE: PLEASE DON"T ACTUALLY HAVE A FILE CALLED unknown.404
    if (retval ==0) { 
        kprintf("%s: opened a file that shouldn't exist\n", __func__);
        uio_close(uio);
        return retval;
    }else if (retval != -EMFILE){ 
        kprintf("%s: failed to open a file, which means we're failing before we can validate the existance of the file\n", __func__);
        return retval;//no reason to close the file if this didn't work out
    } else kprintf("%s: threw the correct error, which is what we wanted\n", __func__);

    return 0;
}
//do I even need this testcases? close is a void return
int test_ktfs_multiclose(){
    //int retval = -EINVAL;
    return 0;
}


int test_ktfs_fetch_dindirection(){

    int retval = -EINVAL;
    int read = 0;
    
   kprintf("reached\n");
    struct uio * uio;
    retval = open_file("c","bee_movie.txt",&uio);
    if (retval <0) {
        kprintf("%s: ktfs failed to open the file successfully\n", __func__);
        return retval; 
    }else kprintf("%s: ktfs opened the file successfully\n", __func__);

    read = uio_read(uio, buff1, BEE_MOVIE_BYTE_LEN/3);
    if (read <0){ 
        kprintf("%s: ktfs failed to read bee_movie into the first buffer\n", __func__);
    };

    read = uio_read(uio, buff2, BEE_MOVIE_BYTE_LEN/3);
    if (read <0){ 
        kprintf("%s: ktfs failed to read bee_movie into the first buffer\n", __func__);
    };

    read = uio_read(uio, buff3, BEE_MOVIE_BYTE_LEN/3);
    if (read <0){ 
        kprintf("%s: ktfs failed to read bee_movie into the first buffer\n", __func__);
    };

   // for (int i = 0; i<3; i++){
   //     for (int j = 0; j<3; j++){
   //         if (i == j) continue;
   //         if (memcmp(buff1, , ));//where I changed my mind lmao
   //     }
   // } 
    retval = memcmp(buff1, buff2, sizeof(buff1));
    if (retval != 0){
        kprintf("%s: buff1 and buff2 should be identical but are not\n",__func__);
        return retval; 
    }

    retval = memcmp(buff1, buff3, sizeof(buff1));
    if (retval != 0){
        kprintf("%s: buff1 and buff3 should be identical but are not\n",__func__);
        return retval; 
    }

    retval = memcmp(buff2, buff3, sizeof(buff3));
    if (retval != 0){
        kprintf("%s: buff1 and buff2 should be identical but are not\n",__func__);
        return retval; 
    }

    kprintf("%s: as long as you calculated dindirection correctly (if not blame sleep deprivation) then this is the intended result\n",__func__);

    uio_close(uio);
    return 0;

}

//[asmuley2@eceb-3026-11 util]$ ./mkfs_ktfs ./../sys/ktfs.raw 8M 103 ./../usr/games/trek ./../usr/random_text/bee_movie.txt ./../usr/random_text/lorem.txt ./../usr/random_text/rand_files/*
int test_ktfs_open_many(){
    int retval = -EINVAL;
    size_t size = strlen("file_0.txt");
    char name[size+3];
    strncpy(name,"file_0.txt", size+1);
    //kprintf("%s\n", name);

    struct uio* uioptr;

    open_file("c", "trek", &uioptr);

    for (int i =0; i < 10; i++){
        name[5] = '0'+i;
        //kprintf("file: %s\n", name);
        
        retval = open_file("c",name, &uioptr);
        if (retval <0) kprintf("failed to find file: %s\n", name);

    }
    name[5] = '1';
    name[6] = '0';
    name[7] = '.';
    name[8] = 't';
    name[9] = 'x';
    name[10] = 't';
    name[11] = 0x0;

    for (int i =10; i < 100; i++){
        if (i%10 ==0) name[5] = '0'+ (i/10);
        name[6] = '0'+i%10;

        //kprintf("file: %s\n", name);
        retval = open_file("c",name, &uioptr);
        if (retval <0) kprintf("failed to find file: %s\n", name);
    }


    name[5] = '1';
    name[6] = '0';
    name[7] = '0';
    name[8] = '.';
    name[9] = 't';
    name[10] = 'x';
    name[11] = 't';
    name[12] = 0x0;

    for (int i =10; i < 26; i++){
        if (i%10 ==0) name[6] = '0'+ (i/10)%10;
        name[7] = '0'+i%10;


        //kprintf("file: %s\n", name);
        retval = open_file("c",name, &uioptr);
        if (retval <0) kprintf("failed to print file: %s\n", name);

    }


    kprintf("opening 100 files actually worked really well\n");
    return 0;

}

int test_ktfs_multiple_uio_reference(){
    int retval;

    struct uio * uioptr1;
    struct uio * uioptr2;


    open_file("c", "trek", &uioptr1); //this should work out, we've done this a thousand times by now

    retval = open_file("c", "trek", &uioptr2);
    if (!(retval == -EBUSY)) {
        uio_close(uioptr1);
        return retval;
    }
    uio_close(uioptr1);
    kprintf("this test supposedly works but I'm just a bit skeptical with not just opting to operate entirely with the refcnt \n");
    return 0;


}

int test_ktfs_setpos_through_fetch(){
   int retval; 

   struct uio * uioptr;
   open_file("c", "bee_movie.txt", &uioptr);
   uio_read(uioptr, buff1, 512);
   uio_read(uioptr, buff1, 512);
   kprintf("%s: reached\n", __func__);

   uint32_t pos = 512;
   uio_cntl(uioptr, FCNTL_SETPOS,&pos);

   kprintf("%s: reached\n", __func__);
   uio_read(uioptr, buff2, 512);

   retval = memcmp(buff1, buff2, 512);
    if (retval != 0){
       kprintf("setpos didn't work correctly\n");
       uio_close(uioptr);
       return retval;
    }

    uio_close(uioptr);
    kprintf("setpos did work correctly here\n");

    return 0;
}

int test_ktfs_getpos_through_fetch(){
   int retval; 

   struct uio * uioptr;
   open_file("c", "bee_movie.txt", &uioptr);
   uio_read(uioptr, buff1, 1000);

   uint32_t pos = 0;
   uio_cntl(uioptr, FCNTL_GETPOS, &pos);

   retval = (pos == 1000);
    if (!retval){
       kprintf("getpos didn't work correctly\n");
       uio_close(uioptr);
       return -1000;
    }

    uio_close(uioptr);
    kprintf("getpos did work correctly here\n");

    return 0;
}

int test_ktfs_getend(){
    
   struct uio * uioptr;
   open_file("c", "bee_movie.txt", &uioptr);

    kprintf("reached get_end basic\n");
   uint32_t end = 0;
   kprintf("addr end: %p\n", &end);  
   kprintf("val end: %d\n", end); 
   uio_cntl(uioptr, FCNTL_GETEND, &end);

   if (end != BEE_MOVIE_BYTE_LEN) {
       kprintf("ktfs_cntl get end isn't right");
       return -10324;
   }


    kprintf("ktfs_cntl get_end is right");
    return 0;


}


int test_ktfs_setpos_past_end(){
   //int retval; 

   struct uio * uioptr;
   open_file("c", "bee_movie.txt", &uioptr);

    kprintf("reached setpos past end\n");
   uint32_t end = 0;
   uio_cntl(uioptr, FCNTL_GETEND, &end);
   
   uint32_t pos = end+4;
   uio_cntl(uioptr, FCNTL_SETPOS, &pos);

   uio_cntl(uioptr, FCNTL_GETPOS, &pos);


    if (pos != end){ 
       uio_close(uioptr);
       kprintf("whatever undefined behavior I thought about but forgot to implement didn't work correctly\n");
       return -10000;
    }

    kprintf("your code behaves correctly in this edgecase. congrats but rethink your life decisions too\n");

    uio_close(uioptr);
    return 0;
}



