#ifndef _TESTSUITE_1_H_
#define _TESTSUITE_1_H_

// Add more test prototypes here
// Add args if you want
void run_testsuite_1(void);
int  test_1(void);

int test_find_storage();
int test_simple_storage_read();
int test_simple_storage_write();
int test_simple_ramdisk_uio_read();
int test_uio_control_ramdisk_read();
// int test_elf_load_with_ramdisk_uio();
int test_cache_get_and_release_block();

#endif // _TESTSUITE_1_H_