// timer.c - A timer system
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "string.h"
#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "misc.h"
//#include "assert.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

// for kfree
#include "heap.h"

#define TWENTYMS (20 * (TIMER_FREQ / 1000))

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}
/* void alarm_init(struct alarm *al, const char *name)
 * Inputs:  struct alarm *al  - Pointer to the alarm object to initialize
 *          const char *name  - Optional name string for the alarm (may be NULL)
 * Outputs: None
 * Return Value: None
 * Function: Initializes the given alarm structure and sets its proper fields.
 *            Initializes condition variable and assigns alarm name if name is not NULL
 *            The twake field is initialized to the current system time to 
 *            track timing for future wake-up or reset events.
 * Side Effects: Modifies the contents of the alarm object pointed to by 'al'.
 */
void alarm_init(struct alarm * al, const char * name) {
    
    // Check to see if name is NULL. Properly intialize the alarm based upon this
    if (name == NULL)
    {
        condition_init(&(al->cond), "default_alarm");
    }
    else
    {
        condition_init(&(al->cond), name);
    }
    al->next = NULL;            // Do NOT add to sleep_list yet. Only initalizes the alarm
    al->twake = rdtime();       // Per documentation, set the twake to the current time
}
/* void alarm_sleep(struct alarm *al, unsigned long long tcnt)
 * Inputs:
 *    struct alarm *al         - Pointer to the alarm object used for sleep and wake-up
 *    unsigned long long tcnt  - Number of ticks to sleep relative to the last 
 *                               alarm initialization, reset, or wake-up
 * Outputs: None
 * Return Value: None
 * Function:  Puts the current thread to sleep on the specified alarm until the 
 *            requested number of ticks (tcnt) have elapsed. If the specified 
 *            duration has already passed, the function returns immediately.
 *            Otherwise, function updates the alarm’s twake time, inserts 
 *            the alarm into the global sleep list in ascending order based
 *            upon twake time and suspends the calling thread until the wake-up time is reached. 
 *            The mtimecmp register is updated to ensure the timer interrupt occurs at the 
 *            next pending alarm event.
 * Side Effects: 
 *    - May modify the global sleep list
 *    - May put the current thread to sleep
 *    - Updates twake and possibly enables timer interrupts.
 */
void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now; struct alarm * prev;
    int pie;
    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;
    
    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;
    
    

    // Critical Section: Interrupts Disabled as modifying the sleep_list
    pie = disable_interrupts();
    // Iterate through alarm sleep_list to find position where to insert the alarm
    struct alarm * head = sleep_list;
    prev = NULL;
    while (head != NULL && head->twake < al->twake)
    {
        prev = head;
        head = head->next;
    }
    if (prev != NULL)
    {
        prev->next = al;
        al->next = head;
    }
    else
    {
        al->next = sleep_list;
        sleep_list = al;
    }
    // set the stcmp
    set_stcmp(sleep_list->twake);                       //  set to the first one of the sleep list
    csrs_sie(RISCV_SIE_STIE);                           // enable timer interrupts as we have added an alarm
    condition_wait(&(al->cond));                        // suspend alarm until wake-up
    restore_interrupts(pie);
}

                                            //DONE: apparently these executions are on the scale of microseconds
void alarm_preempt(){ //lowkey always 20 ms, if on entry to function maybe more depending on how fast rv64 asm instructions execute on qemu???
    trace("%s()", __func__);
    trace("%d", (csrr_sstatus() & RISCV_SSTATUS_SPP));

    struct alarm * pal = kcalloc(1, sizeof(struct alarm));
    alarm_init(pal, "pp"); //specific label given only to preemptive alarms
    
    unsigned long long now; struct alarm * prev;
    int pie;
    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - pal->twake < TWENTYMS)
        pal->twake = UINT64_MAX;
    else
        pal->twake += TWENTYMS;
    
    // If the wake-up time has already passed, return

    if (pal->twake < now)
        return;
    
    
    
    // Critical Section: Interrupts Disabled as modifying the sleep_list
    pie = disable_interrupts();
    // Iterate through alarm sleep_list to find position where to insert the alarm
    struct alarm * head = sleep_list;
    prev = NULL;
    while (head != NULL && head->twake < pal->twake)
    {
        prev = head;
        head = head->next;
    }
    if (prev != NULL)
    {
        prev->next = pal;
        pal->next = head;
    }
    else
    {
        pal->next = sleep_list;
        sleep_list = pal;
    }
    // set the stcmp
    set_stcmp(sleep_list->twake);                       //  set to the first one of the sleep list
    csrs_sie(RISCV_SIE_STIE);                           // enable timer interrupts as we have added an alarm

    //condition_wait(&(pal->cond)) the biggest functional difference between this function and alarm sleep is that this function doesn't condition wait. its a yield alarm not a wake alarm (yes I can cook)
    restore_interrupts(pie);
}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}
/* void handle_timer_interrupt(void)
 * Inputs: None
 * Outputs: None
 * Return Value: None
 * Function: Interrupt service routine (ISR) for the timer. Handles timer 
 *            interrupts by waking any threads whose sleep duration has expired 
 *            and updating the global sleep list accordingly. 
 *            Uses rdtime() to determine the current time and removes all alarms 
 *            whose wake-up time has passed. Threads waiting on those alarms are 
 *            signaled or moved to the ready-to-run list.
 *            After servicing expired alarms, updates the mtimecmp register using 
 *            set_stcmp() to schedule the next timer interrupt for the soonest 
 *            pending alarm. If no alarms remain, disables timer interrupts.
 * Side Effects: 
 *    - Modifies the global sleep list.
 *    - May wake multiple threads
 *    - Updates or disables timer interrupts
 */
void handle_timer_interrupt(void) {
    struct alarm * head = sleep_list;
    struct alarm * next;
    uint64_t now;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());
    // csrs_sie set bit
    // csrc_sie clear bit
    // Check the sleep list and wake all threads whose wake-up time has arrived
    
    int yield_flag = 0;
    while (head != NULL && head->twake <= now)
    {
        if (strncmp(head->cond.name, "pp", 2) == 0) {
            yield_flag = 1;
            next = head->next;
            kfree(head);
            head = next;

        }else {
            condition_broadcast(&(head->cond));
            next = head->next;
            head = next;
        }
    }

    // Check to see if sleep_list is empty. If so disable timer interrupts otherwise timer_isr may 
    // repeatedly be setoff
    sleep_list = head;              // removes expired alarms from sleep_list
    if (head == NULL)
    {
        csrc_sie(RISCV_SIE_STIE);   // disable timer interrupts by clearing timer interrupt bit
    }
    else 
    {
        set_stcmp(sleep_list->twake);     //  set threshold to check to be the first thread whose twake is smallest
    }

        trace("prev priv mode %d", csrr_sstatus() & RISCV_SSTATUS_SPP);
        trace("the thread process address:%p", running_thread_process());
    //IF we came from U-mode and a preemptive alarm went off, then yeet the thread onto the running thread list
    //otherwise its just a noop
    
    if ((csrr_sstatus() & RISCV_SSTATUS_SPP)==0  && yield_flag == 1){
        trace("%d", csrr_sstatus() & RISCV_SSTATUS_SPP);
        running_thread_yield();
    }
    
    // if (running_thread_process()  && yield_flag == 1){
    //     trace("%d", csrr_sstatus() & RISCV_SSTATUS_SPP);
    //     running_thread_yield();
    // }
}
