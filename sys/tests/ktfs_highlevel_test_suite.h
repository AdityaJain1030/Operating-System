#ifndef _KTFSHIGHLEVELTESTSUITE_1_H_
#define _KTFSHIGHLEVELTESTSUITE_1_H_

// Add more test prototypes here
// Add args if you want
void run_ktfs_highlevel_tests(void);
int  test_ktfs_multiopen(void);
int  test_ktfs_unfoundopen(void);
int test_ktfs_multiclose(void);
int test_ktfs_fetch_dindirection(void);
int test_ktfs_multiple_uio_reference(void);
int test_ktfs_setpos_through_fetch(void);//sets the position to the same segment of the file multiple times and makes sure that it reads the same values
int test_ktfs_getpos(void); //not very complicated
int test_ktfs_getend(void); //also not very complicated
int test_ktfs_setpos_past_end(void); //this is actually undefined behavior I'm pretty sure
int test_ktfs_setpos_through_getpos(void); 
int test_ktfs_getpos_through_fetch(void);
int test_ktfs_open_many(void);

int test_ktfs_multi_create(void);
int test_ktfs_delete_free_actual(void);
int test_ktfs_store_precision(void);
#endif // _VIOBLKTESTSUITE_1_H_

