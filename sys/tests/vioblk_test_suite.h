#ifndef _VIOBLKTESTSUITE_1_H_
#define _VIOBLKTESTSUITE_1_H_

// Add more test prototypes here
// Add args if you want
void run_vioblk_tests(void);
int test_vioblk_open_close(void); //opens->closes followed by opens->closes should be successful
int  test_vioblk_double_open(void);//shouldn't be able to open a vioblk device twice
                                   
int test_vioblk_fetch_closed(void); //self explanitory 
int test_vioblk_store_closed(void);

int test_vioblk_cntl_getend(void);
#endif // _VIOBLKTESTSUITE_1_H_

