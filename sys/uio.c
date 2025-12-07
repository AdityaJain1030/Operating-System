/*! @file uio.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌‍‌⁠​⁠⁠‌
    @brief Uniform I/O interface
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef UIO_DEBUG
#define DEBUG
#endif

#ifdef UIO_TRACE
#define TRACE
#endif

#include "uio.h"

#include <stddef.h>  // for NULL and offsetof

#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uioimpl.h"

static void nulluio_close(struct uio* uio);

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz);

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//


// more pipe stuff
// use UIO interface! Underlying UIO object is the PIPE BUFFER!
// Pipe stuff
struct pipe_buffer {
    char* buf;                          // buffer pointer, spec says should be page size
    unsigned long length;                           // current length of buffer/how many are stored
    struct uio* writeuio;              // uio for writing
    struct uio* readuio;               // uio for reading
    unsigned long bufsz;               // size of buffer
    unsigned long head;                 // head is wehre we read, tail is where we write
    unsigned long tail;                 // tiail is where we write
    struct lock lock;               // do we actually need locking??? Art pls answer   
    struct condition not_empty;         // condiiton for pipe intenral buffer not empty
    struct condition not_full;          // condition for pipe internal buffer not full
    int writers_open;
    int readers_open;
};
/*
    Pipe is basically another "UIO" device, but not really. It acts as connector but still interfaces through the uio interface!

    Sepearte writeuio and readuio because we need a way to differentiate
*/

static const struct uio_intf pipe_write_uio_intf = {
    .close = &pipe_write_uio_close,
    .read = NULL,
    .write = &pipe_write_uio_write,
    .cntl = NULL
};


static const struct uio_intf pipe_read_uio_intf = {
    .close = &pipe_read_uio_close,
    .read = &pipe_read_uio_read,
    .write = NULL,
    .cntl = NULL
};




//
// Internal Pipe Functions
// Referenced KTFS style like docs
// we assume that pipebuf is a VALID pointer otherwise it is cooked
int pipefull (struct pipe_buffer* pipebuf)
{
    return pipebuf->length == pipebuf->bufsz;
}

int pipeempty (struct pipe_buffer* pipebuf)
{
    return pipebuf->length == 0;
}




/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */
/*
WHen we close we check if the reader is closed if it is then we can free the underlying memory


*/
void pipe_write_uio_close(struct uio* uio) {
    
    if (uio == NULL) return -EINVAL;


    struct pipe_buffer* pipebuf = (struct pipe_buffer*)((char*)uio - offsetof(struct pipe_buffer, writeuio));


    int free = 0;       // suggestion from ART, keeps code cleaner

    lock_acquire(&pipebuf->lock);
    pipebuf->writers_open--;
    if (pipebuf->writers_open == 0) {
        // Wake up any readers waiting for data
        // this is an edge case, we need to wake them up as otherwise sicne we have 0 writers we have no way to wake up the reader
        // for example say we write "Hello". And then reader reads but then pip_write_uio_close is called. No way to "wake up" reader to return EOF
        // make sure to append EOF?
        int EOF_written = pipe_write_uio_write(pipebuf->writeuio, "", 1); // write EOF character
        if (pipebuf->readers_open > 0 && !pipeempty(pipebuf)) 
        {
            condition_broadcast(&pipebuf->not_empty);
        }
        else    // no readers open, we can free the struct
        {
            free = 1;                                    // free the pipe buffer struct, need the free variable so we dont free before we release the lock
        }
    }
    lock_release(&pipebuf->lock);

    // if not free and writers_open == 0, we rely on reader to CLOSE!
    if (free) 
    {
        free_phys_page((unsigned long)pipebuf->buf);   // free the physical page
        kfree(pipebuf);                                    // free the pipe buffer struct
    }
}


/**
 * @brief Write data from the provided argument buffer into file attached to uio
 * @param uio The file to be written to
 * @param len Number of bytes to write from the buffer to the file
 * @return Number of bytes written from the buffer to the file system if sucessful, negative error
 * code if error
 */
/*

We write byte by byte (I know slow), however we can chunk this if you need to. Just need to be careful wtih edge cases

*/
long pipe_write_uio_write(struct uio* uio, const void *buf, unsigned long buflen) {
    
    if (uio == NULL) return -EINVAL;
    
    struct pipe_buffer* pipebuf = (struct pipe_buffer*)((char*)uio - offsetof(struct pipe_buffer, writeuio));
    
    if (pipebuf->readers_open == 0) 
    {
        return -EPIPE; // No readers, cannot write
    }

    while (pipefull(pipebuf)) 
    {
        // Wait until there is space in the buffer
        // if we say we want to write but the pipe is still open?
        // so many fricking edge cases. 
        /*
            When pipebuf is full, we condaiotn wait on the reader, btu then the reader closes withotu readinf then we need to handlel that

            when reader partially reads some, but then closes and we try to write after we exit then we should reutn bad as well
        
        
        */
        if (pipebuf->readers_open == 0) 
        {
            return -EPIPE; // No readers, cannot write
        }
        condition_wait(&pipebuf->not_full);
    }


    // another check, see above
    if (pipebuf->readers_open == 0) 
    {
        return -EPIPE; // No readers, cannot write
    }


    // pipe buffer is our source buffer, we direclty use uio->open to reference
    unsigned long bytes_written = 0;

    lock_acquire(&pipebuf->lock);

    char* srcbuf = (char*) buf;
    // WE ASSUME THAT USER CAN ONLY SEND IN BYTES LESS THAN 4096! OTHERWISE WE WILL BE BROKEN
    while (bytes_written < buflen) 
    {
        // Wait until there is space in the buffer
        while (pipefull(pipebuf)) 
        {
            if (pipebuf->readers_open == 0) 
            {
                lock_release(&pipebuf->lock);           // reader closed premautrley, return error?
                return -EPIPE; // No readers, stop writing, broken pipe. Should not premautrely close???
            }
            condition_wait(&pipebuf->not_full);
        }

        // Write a byte to the buffer
        pipebuf->buf[pipebuf->tail] = srcbuf[bytes_written];
        pipebuf->tail = (pipebuf->tail + 1) % pipebuf->bufsz;
        bytes_written++;

        // Signal that the buffer is not empty
        condition_signal(&pipebuf->not_empty);
    }
    lock_release(&pipebuf->lock);
    return bytes_written;
}

/*


This may be buggy so idk.

Basically edge case of prematurely closing

*/

void pipe_read_uio_close(struct uio* uio) 
{
    if (uio == NULL) return -EINVAL;
    struct pipe_buffer* pipebuf = (struct pipe_buffer*)((char*)uio - offsetof(struct pipe_buffer, readuio));


    int free = 0;       // suggestion from ART, keeps code cleaner

    lock_acquire(&pipebuf->lock);

    pipebuf->readers_open--;
    if (pipebuf->readers_open == 0)
    {
        // Wake up any writers waiting for space
        // edge case, since, we need to wake them up as otherwise sicne we have 0 readers we have no way to wake up the writer
        // we could end up hangign so be careful!
        // Make sure we either eventually CLOSE writer then or something else! As writes indefnitly
        if (pipebuf->writers_open == 0)
        {
            free = 1;
        }
    }
}

/*


If UIO is null we return -EINVAL

*/


long pipe_read_uio_read(struct uio* uio, void* buf, unsigned long bufsz)
{
    // check if uio is null
    if (uio == NULL || buf == NULL) return -EINVAL;
    
    struct pipe_buffer* pipebuf = (struct pipe_buffer*)((char*)uio - offsetof(struct pipe_buffer, readuio));


    // i think by definiton we return EOF if the pipe is empty and no writers are open
    if (pipebuf->writers_open == 0 && pipeempty(pipebuf)) 
    {
        return 0; // No writers and buffer empty, EOF
    }

    unsigned long bytes_read = 0;

    lock_acquire(&pipebuf->lock);

    char* dstbuf = (char*) buf;

    while (bytes_read < bufsz) 
    {
        // Wait until there is data in the buffer
        while (pipeempty(pipebuf)) 
        {
            if (pipebuf->writers_open == 0) 
            {
                // not sure if we APPEND EOF here
                lock_release(&pipebuf->lock);
                return bytes_read; // No writers, return what we have read so far (could be 0), or do we return EOF?? im not too sure tbh
            }
            condition_wait(&pipebuf->not_empty);
        }

        // Read a byte from the buffer
        dstbuf[bytes_read] = pipebuf->buf[pipebuf->head];
        pipebuf->head = (pipebuf->head + 1) % pipebuf->bufsz;
        bytes_read++;

        // Signal that the buffer is not full
        condition_signal(&pipebuf->not_full);
    }
    // frick idk what to do about EOF
    lock_release(&pipebuf->lock);
    return bytes_read;
}




void uio_close(struct uio* uio) {
    debug("uio_close: refcnt=%d, has_close=%d", uio->refcnt, (uio->intf->close != NULL));

    // Decrement reference count if it's greater than 0
    if (uio->refcnt > 0) {
        uio->refcnt--;
        debug("uio_close: decremented refcnt to %d", uio->refcnt);
    }

    // Only call the actual close method when refcnt reaches 0
    if (uio->refcnt == 0 && uio->intf->close != NULL) {
        debug("uio_close: calling close method");
        uio->intf->close(uio);
    } else if (uio->refcnt > 0) {
        debug("uio_close: NOT calling close (refcnt=%d still has references)", uio->refcnt);
    }
}

long uio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    if (uio->intf->read != NULL) {
        if (0 <= (long)bufsz)
            return uio->intf->read(uio, buf, bufsz);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

long uio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    if (uio->intf->write != NULL) {
        if (0 <= (long)buflen)
            return uio->intf->write(uio, buf, buflen);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

int uio_cntl(struct uio* uio, int op, void* arg) {
    if (uio->intf->cntl != NULL)
        return uio->intf->cntl(uio, op, arg);
    else
        return -ENOTSUP;
}

unsigned long uio_refcnt(const struct uio* uio) {
    assert(uio != NULL);
    return uio->refcnt;
}

int uio_addref(struct uio* uio) { return ++uio->refcnt; }

struct uio* create_null_uio(void) {
    static const struct uio_intf nulluio_intf = {
        .close = &nulluio_close, .read = &nulluio_read, .write = &nulluio_write};

    static struct uio nulluio = {.intf = &nulluio_intf, .refcnt = 0};

    return &nulluio;
}

void create_pipe(struct uio **wptr, struct uio **rptr) {
    // ...
    // we allocate the pipe on the heap??? idk bro I am so LOST
    // intilaize all the inetneral members of the struct
    struct pipe_buffer * pipebuf = kcalloc(1, sizeof(struct pipe_buffer));
    pipebuf->bufsz = PAGE_SIZE;
    pipebuf->buf = (char*) alloc_phys_pages(1);         // we allocate one physical page, since the buffer will be accessed in kenrel we are good nad can direct memory access
    pipebuf->head = 0;
    pipebuf->tail = 0;                                  // initially head and tail are both 0
    pipebuf->writers_open = 1;
    
    pipebuf->readers_open = 1;

    pipebuf->length = 0;


    // bro idk if we lock or not, 
    //lock_init(&pipebuf->lock);
    condition_init(&pipebuf->not_empty, "pipe_not_empty");
    condition_init(&pipebuf->not_full, "pipe_not_full");

    uio_init1(&pipebuf->writeuio, &pipe_write_uio_intf);
    uio_init1(&pipebuf->readuio, &pipe_read_uio_intf);
    
}


static void nulluio_close(struct uio* uio) {
    // ...
}

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // ...
    return -ENOTSUP;
}

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    // ...
    return -ENOTSUP;
}
