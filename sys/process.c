/*! @file process.c
    @brief user process
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

/*!
 * @brief Enables trace messages for process.c
 */
// #include <stdint.h>
// #include <stdlib.h>
// #include <sys/_types.h>
// #include <sys/errno.h>
#ifdef PROCESS_TRACE
#define TRACE
#endif

/*!
 * @brief Enables debug messages for process.c
 */
#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "process.h"

#include "timer.h"
#include "conf.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "trap.h"
#include "uio.h"

// COMPILE-TIME PARAMETERS
//

/*!
 * @brief Maximum number of processes
 */
#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//

static int build_stack(void* stack, int argc, char** argv);

static void fork_func(struct condition* forked, struct trap_frame* tfr);

// INTERNAL GLOBAL VARIABLES
//

/*!
 * @brief The main user process struct
 */
static struct process main_proc;

static struct process* proctab[NPROC] = {&main_proc};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert(memory_initialized && heap_initialized);
    assert(!procmgr_initialized);

    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;
}

int process_exec(struct uio* exefile, int argc, char** argv) {
    // FIXME
    // todo
    // how this works
    // 2. create new page
    // 3. load arguements onto new page
    // 4. create stack on page
    // 5. clear out old page
    // 1. load an elf
    // 6. set SPP (and SPIE almost forgot)
    // 7. call trap jump with pointers to argc argv

    // just look at slides 17 Ill come back to these to fix comments

    // we load it once to test against any issues with the elf, we dont want
    // to unmap the memory space if we dont
    // have a valid elf?
    // since we reset our memory space this will get cleaned up anyway
    // void (*dummy)();
    // int err = elf_load(exefile, &dummy);
    // if (err < 0) {
    //     // our elf is bad
    //     uio_close(exefile); 
    //     return err;
    // }
    // LXDL: 11/26/25
    // Copy Program arguments argc, argv to kernel for this process temporarily
    // Free all user pages (allocated for the old program) KEEP kernel/IO mappings
    //  - calls reset)activemspace. Old program gone. Hence we need to save on KERNEL STACK
    //  - elf_load: load new exectuable image into memory. Make sure to allocate physical pages and update pagetable for new program
    //  - Pass progam argument back to new progam
    // 
    // 
    // 
    // 
    // 


    // 2. create new page
    // actual physical page
    void* newpage = alloc_phys_page();

    // 3. load arguements onto new page
    // 4. create stack on page
    int stack_sz = build_stack(newpage, argc, argv);
    if (stack_sz < 0) 
    {
        free_phys_page(newpage);
        return stack_sz;
    }
    // loook at slides 21
    // 5. clear out old page, i dont think we can access anything not in our stack now
    reset_active_mspace();
    
    // move this down here per slides 17
    // 1. load an elf
    void (*entry_ptr)(); // taken from main i need to learn how elf works still.....
    int err = elf_load(exefile, &entry_ptr);
    uio_close(exefile);

    // i dont think we need this since we already tested the elf above
    if (err < 0) {
        free_phys_page(newpage);

        trace("the error code is %d", err);
        return err;
    }

    // we have to still make the stack discoverable
    // this maps to the last possible address. Its a little weird but this is the layout
    // |      ...     |
    // |              |
    // |              |
    // |              |
    // |              |  going up in memory means we may not be contigious in stack anymore
    // |              |
    // |              |
    // |______________|
    // |    OUR PAGE  | (page_addr)  oxFFFF0000
    // |              |
    // |              |
    // |              |
    // |              |
    // |stacky stack  | (sp) SP GROWS BY DECREMENTING!!!
    // |--------------|
    // |   argc/argv  |
    // |______________| (UMEM_END_VMA) 0xFFFFFFFFFF
    uintptr_t page_addr = UMEM_END_VMA - PAGE_SIZE;
    map_page(page_addr, newpage, PTE_R | PTE_W | PTE_U); // removing X ~~dont think we need X but ill keep it~~ 

    // 6. set SPP (and SPIE almost forgot)
    // csrs_sstatus(RISCV_SSTATUS_SPIE | RISCV_SSTATUS_SPP); // this is wrong
    // trap frame swaps out sstatus with its own, we should just set the trapframe sstatus
    struct trap_frame trap;
    trap.sstatus = csrr_sstatus();
    trap.sstatus &= ~RISCV_SSTATUS_SPP;
    trap.sstatus |= RISCV_SSTATUS_SPIE;

    // set the rest of the trap frame as well
    trap.sp = (void *)(UMEM_END_VMA - stack_sz); // hope this is the right address
    trap.a0 = (long)argc; // ?? this should point to argc location
    trap.a1 = UMEM_END_VMA - stack_sz; // this should point to argv loc, Im pretty sure this is same as sp, but one goes up one goes down
    trap.sepc = entry_ptr;

    // 7. call trap jump with pointers to trap frame and sscratch
    void* kernel_stack = running_thread_stack_base();

    alarm_preempt();
    trap_frame_jump(&trap, kernel_stack); // dunno what to set sscratch to yet

    // how do we free the trap frame??? 

    return 0;
}

int process_fork(const struct trap_frame* tfr) {
    // FIXME
    // dont deal with this till cp3
    // we steal the logic for finding open stuff syscall open
    int pid = -1;
    for (pid = 0; pid <= NPROC; pid++)
    {
        // there are no free procs
        if (pid == NPROC) return -EBUSY;

        // break after pid is found
        if (proctab[pid] == NULL) break;
    }

    struct process *proc = current_process(); 

    struct process *newproc = kcalloc(sizeof(struct process), 1);
    proctab[pid] = newproc;
    //TODO: just realized this but we should be setting the child threads tid somewhere here right?

    // duplicate the fds and increment the uios
    for (int fd = 0; fd < PROCESS_UIOMAX; fd ++)
    {
        if (proc->uiotab[fd] == NULL) continue;
        newproc->uiotab[fd] = proc->uiotab[fd];
        uio_addref(proc->uiotab[fd]); // I think this is the right function
    }

    // duplicate the memspace
    newproc->mtag = clone_active_mspace();
    // u mode calls fork -> we enter s mode fork handler (process_fork) -> process_fork creates a child thread at fork_func using the original tfr (we dont need to clone YET im 99% sure) -> process_fork condition waits for child to start running -> we wait for a few isr thread switches till child starts at fork_func -> child has to DEEP COPY tfr because the actual MEMORY any pointers on tfr->sp will not be accessible (child in UMode will not have the parents vmas) -> child says condition done with the old tfr (but does not return control to parent yet) -> we trap frame jump into child and wait for pre emption to give control back to parent
    // create a new condition 
    struct condition wait_for_child;
    condition_init(&wait_for_child, "fork wait");
    int ctid = spawn_thread(NULL, (void *)fork_func, &wait_for_child, tfr);

    thread_set_process(ctid, newproc);
    newproc->tid = ctid;
    //TODO: condition_wait here for the fork func to finish wiht the trap frame    
    //FIXME: we should be returning child tid 
    condition_wait(&wait_for_child);
    return ctid; 
}

/** \brief
 *
 *
 *  Discard memory space, close your associated uio, free the memory you're supposed to free.
 *
 *
 */
void process_exit(void) {
    // FIXME
    // 1. clear process memory space
    // 2. for each of the uio interfaces the process owns close them if they are open
    // 3. call running_thread_suspend
    struct process* proc = current_process();

    for (int i = 0; i < PROCESS_UIOMAX; i++) {
        if (proc->uiotab[i] != NULL) {
            uio_close(proc->uiotab[i]);
            proc->uiotab[i] = NULL;
        }
    }

    discard_active_mspace();

    thread_set_process(proc->tid, NULL);
    running_thread_exit();
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * \brief Builds the initial user stack for a new process.
 *
 * Builds the stack for a new process, including the argument vector (\p argv)
 * and the strings it points to. Note that \p argv must contain \p argc + 1
 * elements (the last one is a NULL pointer).
 *
 * Remember to round the final stack size up to a multiple of 16 bytes
 * (RISC-V ABI requirement).
 *
 * \param[in,out] stack  Pointer to the stack page (destination buffer).
 * \param[in]     argc   Number of arguments in \p argv.
 * \param[in]     argv   Array of argument pointers; length is \p argc+1 and
 *                       \p argv[argc] must be NULL.
 *
 * \return Size of the stack page on success; negative error code on failure.
 */
int build_stack(void* stack, int argc, char** argv) {
    size_t stksz, argsz;
    uintptr_t* newargv;
    char* p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc) return -ENOMEM;

    stksz = (argc + 1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i]) + 1;
        if (PAGE_SIZE - stksz < argsz) return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert(stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv + argc + 1);

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i]) + 1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

/**
 * \brief Function to be executed by the child process after fork.
 * This is a very beautiful function.
 * Tell the parent process that it is done with the trap frame, then jumps to user space (hint:
 * which function should we use?)
 *
 * \param[in] done  Pointer to a condition variable to signal parent
 * \param[in] tfr   Pointer to a trap frame
 *
 * \return NONE (very important, this is a hint)
 * Im guessing ^^ above hint means brother we never coming baack
 */
void fork_func(struct condition* done, struct trap_frame* tfr) {
    // FIXME
    // see process_fork for comments
    struct trap_frame ktfr;
    memcpy(&ktfr, tfr, sizeof(struct trap_frame));
    condition_broadcast(done);
    // ok we know for sure we need to make it out
    void * kernel_stack = running_thread_stack_base();
    ktfr.a0 = 0; // child returns 0 from fork
    //FIXME: shouldn't we be setting a0 to 0 here? maybe I'm trippin
    alarm_preempt();
    trap_frame_jump(&ktfr, kernel_stack);
}
