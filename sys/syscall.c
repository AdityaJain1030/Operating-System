/*! @file syscall.c
    @brief system call handlers
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "memory.h"
#include "misc.h"
#include "process.h"
#include "scnum.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "uio.h"

// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame *tfr);  // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame *tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char **argv);
static int sysfork(const struct trap_frame *tfr);
static int syswait(int tid);
static int sysprint(const char *msg);
static int sysusleep(unsigned long us);

static int sysfsdelete(const char *path);
static int sysfscreate(const char *path);

static int sysopen(int fd, const char *path);
static int sysclose(int fd);
static long sysread(int fd, void *buf, size_t bufsz);
static long syswrite(int fd, const void *buf, size_t len);
static int sysfcntl(int fd, int cmd, void *arg);
static int syspipe(int *wfdptr, int *rfdptr);
static int sysuiodup(int oldfd, int newfd);

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Initiates syscall present in trap frame struct and stores the return address into the sepc
 * @details sepc will be used to return back to program execution after interrupt is handled and
 * sret is called
 * @param tfr pointer to trap frame struct
 * @return void
 */

void handle_syscall(struct trap_frame *tfr) {
    // since we called exception, we want to go to
    // the next instruction
    tfr->sepc += 4;
    // return the result of the syscall
    tfr->a0 = syscall(tfr);
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Calls specified syscall and passes arguments
 * @details Function uses register a7 to determine syscall number and arguments are passed in from
 * a0-a5 depending on the function
 * @param tfr pointer to trap frame struct
 * @return result of syscall
 */

int64_t syscall(const struct trap_frame *tfr) {
    switch (tfr->a7) {
        case SYSCALL_EXIT:
            return sysexit();
        case SYSCALL_EXEC:
            return sysexec((int)tfr->a0, (int)tfr->a1, (char **)tfr->a2);
        case SYSCALL_FORK:
            return sysfork(tfr);
        case SYSCALL_WAIT:
            return syswait((int)tfr->a0);
        case SYSCALL_PRINT:
            return sysprint((const char *)tfr->a0);
        case SYSCALL_USLEEP:
            return sysusleep((unsigned long)tfr->a0);
        case SYSCALL_FSCREATE:
            return sysfscreate((const char *)tfr->a0);
        case SYSCALL_FSDELETE:
            return sysfsdelete((const char *)tfr->a0);
        case SYSCALL_OPEN:
            return sysopen((int)tfr->a0, (const char *)tfr->a1);
        case SYSCALL_CLOSE:
            return sysclose((int)tfr->a0);
        case SYSCALL_READ:
            return sysread((int)tfr->a0, (void *)tfr->a1, (size_t)tfr->a2);
        case SYSCALL_WRITE:
            return syswrite((int)tfr->a0, (const void *)tfr->a1, (size_t)tfr->a2);
        case SYSCALL_FCNTL:
            return sysfcntl((int)tfr->a0, (int)tfr->a1, (void *)tfr->a2);
        case SYSCALL_PIPE:
            return syspipe((int *)tfr->a0, (int *)tfr->a1);
        case SYSCALL_UIODUP:
            return sysuiodup((int)tfr->a0, (int)tfr->a1);
        default:
            return -ENOTSUP;
    }
 }

/**
 * @brief Calls process exit
 * @return void
 */

int sysexit(void) { 
    process_exit();
    return 0; 
}

/**
 * @brief Executes new process given a executable and arguments
 * @details Valid fd checks, get current process struct, close fd being executed, finally calls
 * process_exec with arguments and executable io "file"
 * @param fd file descripter idx
 * @param argc number of arguments in argv
 * @param argv array of arguments for multiple args
 * @return result of process_exec, else -EBADFD on invalid file descriptors
 */

int sysexec(int fd, int argc, char **argv) {
    // what to do here, check if file descriptor is valid
    // check if current process is allowed to access file descriptor
    // check if argc is valid wrt argv
    // check if argv is in the space of the user

    // call process exec
    // if process exec returns that means the file was invalid
    // return the error code

    // check if process fd is valid
    if (fd < 0) return -EBADFD;
    if (fd >= PROCESS_UIOMAX) return -EBADFD;
    
    // check if args are valid
    if (validate_vptr(argv, sizeof(char*) * (argc+1), PTE_U | PTE_X | PTE_R) != 0) return -EINVAL;

    for (int i = 0; i < argc; i++)
    {
        if (validate_vstr(argv[i], PTE_R | PTE_W | PTE_U) != 0) return -EINVAL;
    }

    struct process *running = current_process();
    if (running->uiotab[fd] == NULL) return -EBADFD;

    struct uio *exefile = running->uiotab[fd];
    // process_exec does not return!
    running->uiotab[fd] = NULL;

    // call process_exec
    int err = process_exec(exefile, argc, argv);

    // if we returned something went wrong and we should tell the 
    // calling program
    return err;
}

/**
 * @brief Forks a new child process using process_fork
 * @param tfr pointer to the trap frame
 * @return result of process_fork
 */

int sysfork(const struct trap_frame *tfr) {
    //cp3
    return 0;
}

/**
 * @brief Sleeps till a specified child process completes
 * @details Calls thread_join with the thread id the process wishes to wait for
 * @param tid thread_id
 * @return result of thread_join else invalid on invalid thread id
 */

int syswait(int tid) { 
    // cp3
    return 0;
}

/**
 * @brief Prints to console via kprintf
 * @details Validates that msg string is valid via validate_vstr and pages are mapped, calls kprintf
 * on current running process
 * @param msg string msg in userspace
 * @return 0 on sucess else error from validate_vstr
 */

int sysprint(const char *msg) {
    // check if string is valid vmem address
    // print
    int valid = validate_vstr(msg, PTE_U);
    if (valid != 0) return valid; // return invalid reason
   
    //print
    // kprintf(msg);
    kprintf("Thread <%s:%d> says: %s\n",
        thread_name(running_thread()),
        running_thread(),
        msg);

    return 0; 
}

/**
 * @brief Sleeps process till specificed amount of time has passed
 * @details Creates alarm struct, inits struct with name usleep, which sets the current time via the
 * rd_time() function, taking values from the csr, makes frequency calcuation to determine us has
 * passed before waking process
 * @param us time in us for process to sleep
 * @return 0
 */

int sysusleep(unsigned long us) {
    // we dont have to do any checking here thankfully
    struct alarm alarm;
    alarm_init(&alarm, "sleep");
    alarm_sleep_us(&alarm, us);
    return 0;
}

/**
 * @brief Creates a new file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * create_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfscreate(const char *path) {
    // validate path
    // parse the path
    // call ktfs to create the file
    int valid = validate_vstr(path, PTE_U);
    if (valid != 0) return valid;
    
    char *mpnameptr;
    char *flnameptr;
    
    valid = parse_path(path, &mpnameptr, &flnameptr);
    if (valid != 0){
        kfree(mpnameptr);
        kfree(flnameptr);
        return valid;
    }

    valid = create_file(mpnameptr, flnameptr);

    kfree(mpnameptr);
    kfree(flnameptr);

    return valid;
}

/**
 * @brief Deletes a file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * delete_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfsdelete(const char *path) {
    // validate path
    // parse the path
    // call ktfs to delete the file
    int valid = validate_vstr(path, PTE_U);
    if (valid != 0) return valid;
    
    char *mpnameptr;
    char *flnameptr;
    
    valid = parse_path(path, &mpnameptr, &flnameptr);
    if (valid != 0){
        kfree(mpnameptr);
        kfree(flnameptr);
        return valid;
    }

    valid = delete_file(mpnameptr, flnameptr);

    kfree(mpnameptr);
    kfree(flnameptr);

    return valid;
}

/**
 * @brief Opens a file or device of specified fd for given process
 * @details gets current process, allocates file descriptor (if fd = -1) or uses valid file
 * descriptor given, validates and parses user provided path, calls open_file
 * @param fd file descriptor number
 * @param path User provided path string
 * @return fd number if sucessful else return error that occured -EMFILE or -EBADFD
 */

int sysopen(int fd, const char *path) {
    // check if the fd is valid
    // then check if path is valid
    // then if the fd is valid we can create it
    
    // fd check
    if (fd < -1) return -EBADFD;
    if (fd >= PROCESS_UIOMAX) return -EBADFD;
    
    struct process *running = current_process();
    // if (running->uiotab[fd] != NULL) return -EBADFD;

    // path check
    int val = validate_vstr(path, PTE_U | PTE_R);
    if (val != 0) return val;

    // find first free fd
    if (fd == -1)
    {
        for (fd = 0; fd <= PROCESS_UIOMAX; fd++)
        {
            // there are no free files
            if (fd == PROCESS_UIOMAX) return -EMFILE;

            // break after fd is found
            if (running->uiotab[fd] == NULL) break;
        }
    }
    // we keep this here for the fd != -1 case, in which we need to see if the user provided
    // fd is busy
    if (running->uiotab[fd] != NULL) return -EBADFD;

    // everything looks good we can allocate the file
    char *mpnameptr;
    char *flnameptr;

    val = parse_path(path, &mpnameptr, &flnameptr);
    if (val != 0) return val;

    // taken from main.c
    struct uio *file;
    val = open_file(mpnameptr, flnameptr, &file);
    if (val != 0) return val;

    kfree(mpnameptr);
    kfree(flnameptr);
    
    running->uiotab[fd] = file;
    return fd;
}

/**
 * @brief Closes file or device of specified fd for given process
 * @details gets current process, calls close function of the io, deallocates the file descriptor
 * @param fd file descriptor
 * @return 0 on success, error on invalid file descriptor or empty file descriptor
 */

int sysclose(int fd) { 
    // check if the fd is valid
    // then if it is we close it

    if (fd < 0) return -EBADFD;
    if (fd >= PROCESS_UIOMAX) return -EBADFD;
    struct process *running = current_process();
    if (running->uiotab[fd] == NULL) return -ENOENT;

    uio_close(running->uiotab[fd]);
    return 0;

}

/**
 * @brief Calls read function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call ioread with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param bufsz number of bytes to be read
 * @return number of bytes read
 */

long sysread(int fd, void *buf, size_t bufsz) { return 0; }

/**
 * @brief Calls write function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call iowrite with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param len number of bytes to be written
 * @return number of bytes written
 */

long syswrite(int fd, const void *buf, size_t len) { return 0; }

/**
 * @brief Calls device input output commands for a given device instance
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * ensure that fcntl type exists, validate argument pointer, issue fcntl
 * @param fd file descriptor number
 * @param cmd selection of fcntl
 * @param arg pointer to arguments
 * @return number of bytes written
 */

int sysfcntl(int fd, int cmd, void *arg) { return 0; }

/**
 * @brief Creates a pipe for the current process
 * @details The function retrieves the current process. If either the write or read descriptor
 * pointer stores a negative value, an unused descriptor is assigned. If both file descriptors are
 * unused and valid, the function connects them via create_pipe function.
 * @param wfdptr pointer to write file descriptor
 * @param rfdptr pointer to read file descriptor
 * @return 0 on success. Else, negative error code on invalid file descriptor, or if a file
 * descriptor is already in use, or if no descriptors are found available.
 */
int syspipe(int *wfdptr, int *rfdptr) { return 0; }

/**
 * @brief Duplicates a file description
 * @details Allocates a new file descriptor that refers to the same open _uio_ as the descriptor
 * _oldfd_. Increments the _refcnt_ if successful.
 * @param oldfd old file descriptor number
 * @param newfd new file descriptor number
 * @return fd number if sucessful else return error on invalid file descriptor or empty file
 * descriptor
 */

int sysuiodup(int oldfd, int newfd) { return 0; }