// thread.c - Threads
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

/*! @file thread.c
    @brief Thread manager and operations
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef THREAD_TRACE
#define TRACE
#endif

#ifdef THREAD_DEBUG
#define DEBUG
#endif

#include "thread.h"

#include <stddef.h>
#include <stdint.h>

#include "assert.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "error.h"

#include <stdarg.h>

// COMPILE-TIME PARAMETERS
//

// NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

// EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//


enum thread_state {
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_SELF,                                //   same as thread_running
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context {
    union {
        uint64_t s[12];
        struct {
            uint64_t a[8];                      // s0, ..., s7
            void (*pc)(void);                   // s8: Holds thread_exit during thread_spawn()
            uint64_t _pad;                      // s9
            void * fp;                          // s10 Frame pointer points start of the THREAD stack frame
            void * ra;                          // s11 Points to entry function during thread_spawn()
        } startup;
    };

    void * ra;                                  //  points to _thread_startup()
    void * sp;                                  //  thread stack pointer
};

struct thread_stack_anchor {
    struct thread * ktp;                        // pointer to thread structure
    void * kgp;
};

struct thread {
    struct thread_context ctx;                      // must be first member (thrasm.s)
    int id; // index into thrtab[]
    enum thread_state state;             
    const char * name;
    struct thread_stack_anchor * stack_anchor;      //  base of every threads stack
    void * stack_lowest;                            // 
    struct process * proc;                          // 
    struct thread * parent;
    struct thread * list_next;
    struct condition * wait_cond;
    struct condition child_exit;
    struct lock * lock_list;                        // mp3
};

// INTERNAL MACRO DEFINITIONS
// 

// Pointer to running thread, which is kept in the tp (x4) register.

#define TP ((struct thread*)__builtin_thread_pointer())

// Macro for changing thread state. If compiled for debugging (DEBUG is
// defined), prints function that changed thread state.

#define set_thread_state(t,s) do { \
    debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
        (t)->name, (t)->id, \
        thread_state_name((t)->state), \
        thread_state_name(s), \
        TP->name, TP->id, \
        __func__); \
    (t)->state = (s); \
} while (0)

// INTERNAL FUNCTION DECLARATIONS
//

// Initializes the main and idle threads. called from threads_init().

static void init_main_thread(void);
static void init_idle_thread(void);

// Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread * thr);

// Returns a string representing the state name. Used by debug and trace
// statements, so marked unused to avoid compiler warnings.

static const char * thread_state_name(enum thread_state state)
    __attribute__ ((unused));

// void thread_reclaim(int tid)
//
// Reclaims a thread's slot in thrtab and makes its parent the parent of its
// children. Frees the struct thread of the thread.

static void thread_reclaim(int tid);

// struct thread * create_thread(const char * name)
//
// Creates and initializes a new thread structure. The new thread is not added
// to any list and does not have a valid context (_thread_switch cannot be
// called to switch to the new thread).

static struct thread * create_thread(const char * name);

// void running_thread_suspend(void)
// Suspends the currently running thread and resumes the next thread on the
// ready-to-run list using _thread_swtch (in threasm.s). Must be called with
// interrupts enabled. Returns when the current thread is next scheduled for
// execution. If the current thread is TP, it is marked READY and placed
// on the ready-to-run list. Note that running_thread_suspend will only return if the
// current thread becomes READY.

static void running_thread_suspend(void);

void lock_release_completely(struct lock * lock);

// void release_all_thread_locks(struct thread * thr)
// Releases all locks held by a thread. Called when a thread exits.

static void release_all_thread_locks(struct thread * thr);

// The following functions manipulate a thread list (struct thread_list). Note
// that threads form a linked list via the list_next member of each thread
// structure. Thread lists are used for the ready-to-run list (ready_list) and
// for the list of waiting threads of each condition variable. These functions
// are not interrupt-safe! The caller must disable interrupts before calling any
// thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list * list);
static int tlempty(const struct thread_list * list);
static void tlinsert(struct thread_list * list, struct thread * thr);
static struct thread * tlremove(struct thread_list * list);
static void tlappend(struct thread_list * l0, struct thread_list * l1);

static void idle_thread_func(void);

// IMPORTED FUNCTION DECLARATIONS
// defined in thrasm.s
//

extern struct thread * _thread_swtch(struct thread * thr);

extern void _thread_startup(void);

// INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR-1)

static struct thread main_thread;
static struct thread idle_thread;

extern char _main_stack_lowest[]; // from start.s
extern char _main_stack_anchor[]; // from start.s

static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_SELF,
    .stack_anchor = (void*)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit"
};

extern char _idle_stack_lowest[]; // from thrasm.s
extern char _idle_stack_anchor[]; // from thrasm.s

static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void*)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = &_thread_startup,
    // FIXME your code goes here
    .ctx.startup.ra = &idle_thread_func
};

static struct thread * thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread
};

static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread
};

// EXPORTED FUNCTION DEFINITIONS
//


int running_thread(void) {
    return TP->id;
}

void thrmgr_init(void) {
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}

/*
 * int spawn_thread(const char *name, void (*entry)(void), ...)
 * Inputs:  const char *name     - Name of the thread to spawn
 *          void (*entry)(void)  - Function that the thread will execute
 *          ...                  - Variable arguments (for entry function)
 * Outputs: int                  - Thread ID on success, -1 on failure
 * Description: Creates and spawns a new thread with the given name that will
 *              execute the specified function. Allocates memory for full thread struct
 *              and its stack frame. Initializes thread context and 
 *              adds it to the ready queue. DOES NOT immediately run thread
 * Side Effects: Changes ready list by appending the current thread to it. 
*/

int spawn_thread (
    const char * name,
    void (*entry)(void),
    ...)
{
    struct thread * child;
    va_list ap;
    int pie;
    int i;

    child = create_thread(name);                                        // returns pointer to created thread that has parent, stack_anchor initialized

    if (child == NULL)
        return -EMTHR;

    set_thread_state(child, THREAD_READY);          

    // critical section: modifying ready_list
    pie = disable_interrupts();                         
    tlinsert(&ready_list, child);                          
    restore_interrupts(pie);

   // filling in entry function arguments is given below, the rest is up to you

    va_start(ap, entry);
    for (i = 0; i < 8; i++)
        child->ctx.startup.a[i] = va_arg(ap, uint64_t);                 // addiitonal arguments for the function of entry
    va_end(ap);

    /*
        WE DO NOT CALL THREAD_STARTUP! The switch function calls _thread_startup the first time swtch_context is called.
        Switch function->start up->entry function->exit
    */
    child->ctx.startup.ra = entry;                                      //  put entry into s11
    child->ctx.ra = _thread_startup;                                    // put _start_up into position 13/13th member of context
    child->ctx.startup.pc = running_thread_exit;                        //  put thread exit into s8
    child->ctx.sp = (void*)((uint64_t)child->stack_anchor);             // stack anchor is base of stack (highest address)
    child->ctx.startup.fp = (void*)((uint64_t)child->stack_anchor);
    return child->id;
}

/*
 * void running_thread_exit(void)
 * Inputs: None
 * Outputs: None
 * Description: Exits the currently running thread. If the main thread is exiting,
 *              halts the system with success. Otherwise, marks the thread as exited,
 *              notifies the parent thread, and suspends execution.
 * Side Effects: Broadcasts to parent thread that may be possibly waiting for child thread to exit
*/

void running_thread_exit(void) {
    if (TP->id == MAIN_TID)
    {
        halt_success();                                 
    }
    else
    {
        TP->state = THREAD_EXITED;
    }
    condition_broadcast(&TP->parent->child_exit);           // broadcast to parent

    running_thread_suspend();                               // suspends exiting thread permanently. Should not reach halt_failure() 

    halt_failure();
}


void running_thread_yield(void) {
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}

/*
 * int thread_join(int tid)
 * Inputs: int tid - Thread ID to wait for (0 to wait for any child thread)
 * Outputs: int - Thread ID of the joined thread on success, negative error code on failure
 * Description: Waits for a child thread to exit. If tid is not zero and the child exists, then the thread (parent) waits for that
 *              specific child thread. If tid is zero and children exist, waits for any child thread to exit.
 *              If there is a child that exited, it reclaims the child thread's resources and returns its thread ID.
 * Side Effects: May free child thread struct
 */

int thread_join(int tid) {
    int hasChildren = 0;

    //  Loop through thrtab table to check if current thread has any children, if not return -EINVAL
    for (int ctid = 1; ctid < NTHR; ctid++) 
    {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == TP)
        {
            hasChildren = 1;
        }
    }
    if (tid == 0 && hasChildren == 0) return -EINVAL;       // Does not have children
    if (tid < 0 || tid >= 16) return -EINVAL;               // Invalid TID

    // Check to see if child exists and is actually a child
    if (tid != 0)
    {
        if (thrtab[tid] == NULL || thrtab[tid]->parent != TP)
        {
            return -EINVAL;
        }
    }

    int threadToReclaim = tid;          // Variable that holds reclaimed child thread (if any)


    //  Join on specific Child
    if (tid != 0)
    {
        // Possible critical section. If interrupts somehow change a child's state
        int pie = disable_interrupts();
        while (thrtab[tid]->state != THREAD_EXITED)
        {
            condition_wait(&(TP->child_exit));              //  condition wait on child_exit
        }
        restore_interrupts(pie);
    }
    else    // Join on any child
    {
        int child_exited = 0;
        int pie = disable_interrupts();
        while (child_exited == 0)
        {
            for (int ctid = 1; ctid < NTHR; ctid++)         // Loop through all possible threads
            {
                // Check to make sure thread actually exists and is a child
                if (thrtab[ctid] != NULL && thrtab[ctid]->parent == TP && thrtab[ctid]->state == THREAD_EXITED)
                {
                    child_exited = 1;
                    threadToReclaim = ctid;
                }
            }
            //  If no child has exited, continue to condition_wait
            if (child_exited == 0)
            {
                condition_wait(&(TP->child_exit));
            }
        }
        restore_interrupts(pie);
    }
    thread_reclaim(threadToReclaim);            // Frees resources used by child thread
    return threadToReclaim;
}

struct process * thread_process(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->proc;
}

struct process * running_thread_process(void) {
    return TP->proc;
}

void thread_set_process(int tid, struct process * proc) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    thrtab[tid]->proc = proc;
}

void thread_detach(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    thrtab[tid]->parent = NULL;
}

const char * thread_name(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

const char * running_thread_name(void) {
    return TP->name;
}

void * running_thread_stack_base(void){
    return TP->stack_anchor;
}

void condition_init(struct condition * cond, const char * name) {
    tlclear(&cond->wait_list);
    cond->name = name;
}

void condition_wait(struct condition * cond) {
    int pie;

    trace("%s(cond=<%s>) in <%s:%d>", __func__,
        cond->name, TP->name, TP->id);

    assert(TP->state == THREAD_SELF);

    // Insert current thread into condition wait list
    
    set_thread_state(TP, THREAD_WAITING);


    TP->wait_cond = cond;
    TP->list_next = NULL;
    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);

    running_thread_suspend();
}


/* void condition_broadcast(struct condition *cond)
 * Inputs: struct condition *cond - Pointer to the condition variable to broadcast on
 * Outputs: None
 * Return Value: None
 * Function: Wakes up all threads waiting on the given condition variable.
 *            This function may be called from an ISR. It does NOT cause a
 *            context switch from the currently running thread. Waiting threads
 *            are added to the ready-to-run list in the same order they were
 *            added to the condition’s wait queue (based upon round-robin scheduling)
 * Side Effects: Moves all waiting threads from the condition wait queue to the
 *               ready-to-run list. Does not yield or switch context.
 */
void condition_broadcast(struct condition * cond) {
    // FIXME your code goes here
    // need to disable interrupts while broadcasting
    //kprintf("Broadcastin this conditino: %s\n", cond->name);

    // Critical Section: Disable interrupts while modifying the ready list.
    int pie = disable_interrupts();
    struct thread * head = cond->wait_list.head;

    // Set all threads waiting on condition to ready
    while (head != NULL)
    {
        head->state = THREAD_READY;
        head = head->list_next;
        
    }
    tlappend(&ready_list, &(cond->wait_list));          //   Append all threads waiting on condition to ready list using round-robin scheduling
    restore_interrupts(pie);
}

void lock_init(struct lock * lock) {
    memset(lock, 0, sizeof(struct lock));
    condition_init(&lock->release, "lock_release");
}

void lock_acquire(struct lock * lock) {
    if (lock->owner != TP) {
        while (lock->owner != NULL)
            condition_wait(&lock->release);
        
        lock->owner = TP;
        lock->cnt = 1;
        lock->next = TP->lock_list;
        TP->lock_list = lock;
    } else
        lock->cnt += 1;
}

void lock_release(struct lock * lock) {
    assert (lock->owner == TP);
    assert (lock->cnt != 0);

    lock->cnt -= 1;

    if (lock->cnt == 0)
        lock_release_completely(lock);
}

// INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void) {
    // Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
}

void init_idle_thread(void) {
    // Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
}

static void set_running_thread(struct thread * thr) {
    asm inline ("mv tp, %0" :: "r"(thr) : "tp");
}

const char * thread_state_name(enum thread_state state) {
    static const char * const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_SELF] = "SELF",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"
    };

    if (0 <= (int)state && (int)state < sizeof(names)/sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void thread_reclaim(int tid) {
    struct thread * const thr = thrtab[tid];
    int ctid;

    assert (0 < tid && tid < NTHR && thr != NULL);
    assert (thr->state == THREAD_EXITED);

    // Make our parent thread the parent of our child threads. We need to scan
    // all threads to find our children. We could keep a list of all of a
    // thread's children to make this operation more efficient.

    for (ctid = 1; ctid < NTHR; ctid++) {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;
    kfree(thr);
}

struct thread * create_thread(const char * name) {
    struct thread_stack_anchor * anchor;
    void * stack_lowest;
    size_t stack_size;
    struct thread * thr;
    int tid;

    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);

    // Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;
    
    if (tid == NTHR)
        return NULL;
    
    // Allocate a struct thread and a stack

    thr = kcalloc(1, sizeof(struct thread));
    
    stack_size = 4000; // change to PAGE_SIZE in mp3
    stack_lowest = kmalloc(stack_size);
    anchor = stack_lowest + stack_size;
    anchor -= 1; // anchor is at base of stack
    thr->stack_lowest = stack_lowest;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;

    thrtab[tid] = thr;

    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    thr->proc = TP->proc;
    return thr;
}
/* void running_thread_suspend(void)
 * Inputs: None
 * Outputs: None
 * Return Value: None
 * Function: Suspends the currently running thread and switches to the next
 *            ready thread on the ready-to-run list using _thread_swtch in thrasm.s.
 *            Must be called with interrupts enabled. If current thread is in THREAD_SELF 
 *            (THREAD_RUNNING) mark as THREAD_READY and place back on the ready-to-run list 
 *            This function only returns when the current thread is scheduled again for execution.
 * Side Effects: May change the running thread context. Modifies thread states and
 *               the ready-to-run list. May also free exiting thread stack.
 */
void running_thread_suspend(void) {
    // FIXME your code goes here

    // Critical Section: Modifying the ready_list. 
    int pie = disable_interrupts();
    struct thread * next;
    next = tlremove(&ready_list);

    // If thread is currently running, add to ready list
    if (TP->state == THREAD_SELF)
    {
        TP->state = THREAD_READY;
        tlinsert(&ready_list, TP);
    }
    

    /*
    
    Thread A = Thread that called suspend
    Thread B = Thread to switch to


    Flow:
    A -> B -> (In B suspend)

    _thread_swtch returns thread A. Thread B exectues the line immediately 
    following the call of _thread_swtch
        
    */
    enable_interrupts();                                    // Documentation requires that we must enable interrupts before calling _thread_swtch
    next->state = THREAD_SELF;
    struct thread* old = _thread_swtch(next);
    if (old->state == THREAD_EXITED)
    {
        kfree(old->stack_lowest);                           // free the THREAD's stack!
    }
    restore_interrupts(pie);                                // after thread_swtch returns, we are in need "switched" thread, hence we can restore interrrupts
}

void tlclear(struct thread_list * list) {
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list * list) {
    return (list->head == NULL);
}

void tlinsert(struct thread_list * list, struct thread * thr) {
    thr->list_next = NULL;
    if (thr == NULL)
        return;

    if (list->tail != NULL) {
        assert (list->head != NULL);
        list->tail->list_next = thr;
    } else {
        assert(list->head == NULL);
        list->head = thr;
    }
    list->tail = thr;
}

struct thread * tlremove(struct thread_list * list) {
    struct thread * thr;

    thr = list->head;
    
    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;
    
    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

/*
Appends l1 to l0

Tail refers last non-null node


*/
void tlappend(struct thread_list * l0, struct thread_list * l1) {
    if (l0->head != NULL) {
        assert(l0->tail != NULL);
        
        if (l1->head != NULL) {
            assert(l1->tail != NULL);
            l0->tail->list_next = l1->head;
            l0->tail = l1->tail;
        }
    } else {
        assert(l0->tail == NULL);
        l0->head = l1->head;
        l0->tail = l1->tail;
    }

    l1->head = NULL;
    l1->tail = NULL;
}

void lock_release_completely(struct lock * lock) {
    struct lock ** hptr;

    condition_broadcast(&lock->release);
    hptr = &TP->lock_list;
    while (*hptr != lock && *hptr != NULL)
        hptr = &(*hptr)->next;
    assert (*hptr != NULL);
    *hptr = (*hptr)->next;
    lock->owner = NULL;
    lock->next = NULL;
}

void release_all_thread_locks(struct thread * thr) {
    struct lock * head;
    struct lock * next;

    head = thr->lock_list;

    while (head != NULL) {
        next = head->next;
        head->next = NULL;
        head->owner = NULL;
        head->cnt = 0;
        condition_broadcast(&head->release);
        head = next;
    }

    thr->lock_list = NULL;
}

void idle_thread_func(void) {
    // The idle thread sleeps using wfi if the ready list is empty. Note that we
    // need to disable interrupts before checking if the thread list is empty to
    // avoid a race condition where an ISR marks a thread ready to run between
    // the call to tlempty() and the wfi instruction.

    for (;;) {
        // If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            running_thread_yield();
        
        // No runnable threads. Sleep using the wfi instruction. Note that we
        // need to disable interrupts and check the runnable thread list one
        // more time (make sure it is empty) to avoid a race condition where an
        // ISR marks a thread ready before we call the wfi instruction.

        disable_interrupts();
        if (tlempty(&ready_list))
            asm ("wfi");
        enable_interrupts();
    }
}