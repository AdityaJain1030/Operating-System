// uart.c -  NS8250-compatible serial port
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "plic.h"
#include <stddef.h>
#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "misc.h"
#include "uart.h"
#include "devimpl.h"
#include "intr.h"
#include "heap.h"
#include "thread.h"
#include "console.h"

#include "error.h"

#include <stdint.h>

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_DEVNAME
#define UART_DEVNAME "uart"
#endif


// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

// Simple fixed-size ring buffer

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

// UART device structure

struct uart_serial {
    struct serial base;
    volatile struct uart_regs * regs;
    int irqno;
    char opened;

    unsigned long rxovrcnt; ///< number of times OE was set
    
    struct condition rxbnotempty; ///< signalled when rxbuf becomes not empty
    struct condition txbnotfull;  ///< signalled when txbuf becomes not full

    struct ringbuf rxbuf;
    struct ringbuf txbuf;
    struct lock rq_lock ; // lock
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_serial_open(struct serial * ser);
static void uart_serial_close(struct serial * ser);
static int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);
static int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz);

static void uart_isr(int srcno, void * aux);

// Ring buffer (struct rbuf) functions

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf uart_serial_intf = {
    .blksz = 1,
    .open = &uart_serial_open,
    .close = &uart_serial_close,
    .recv = &uart_serial_recv,
    .send = &uart_serial_send
};

// EXPORTED FUNCTION DEFINITIONS
// 


void attach_uart(void * mmio_base, int irqno) {
    struct uart_serial * uart;

    trace("%s(%p,%d)", __func__, mmio_base, irqno);
    
    // UART0 is used for the console and should not be attached as a normal
    // device. It should already be initialized by console_init(). We still
    // register the device (to reserve the name uart0), but pass a NULL device
    // pointer, so that find_serial("uart", 0) returns NULL.

    if (mmio_base == (void*)UART0_MMIO_BASE) {
        register_device(UART_DEVNAME, DEV_SERIAL, NULL);
        return;
    }
    
    uart = kcalloc(1, sizeof(struct uart_serial));

    uart->regs = mmio_base;
    uart->irqno = irqno;
    uart->opened = 0;

    // Initialize condition variables. The ISR is registered when our interrupt
    // source is enabled in uart_serial_open().

    condition_init(&uart->rxbnotempty, "uart.rxnotempty");
    condition_init(&uart->txbnotfull, "uart.txnotfull");
    lock_init(&uart->rq_lock);

    // Initialize hardware

    uart->regs->ier = 0;
    uart->regs->lcr = LCR_DLAB;
    // fence o,o ?
    uart->regs->dll = 0x01;
    uart->regs->dlm = 0x00;
    // fence o,o ?
    uart->regs->lcr = 0; // DLAB=0

    serial_init(&uart->base, &uart_serial_intf);
    register_device(UART_DEVNAME, DEV_SERIAL, uart);
}

int uart_serial_open(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);
    if (uart->opened)
    {
        return -EBUSY;
    }
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // Enable interrupts when data ready (DR) status asserted
    uart->regs->ier = IER_DRIE;
  
    //irqno is also srcno, set prio to some non-zero, isr function, aux is just the serial struct
    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart);

    uart->opened = 1; //marks the device as opened 
    return 0;
}

void uart_serial_close(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);
    if(!uart->opened) 
    {
        return; //if the serial hasn't been opened yet this function does nothing.
    }

    //uart->regs->ier= 0; //disables all interrupts from the device by clearing the IER register. (disable_intr_source doesn't)

    disable_intr_source(uart->irqno);
    //disable_intr_source disables the source on PLIC
    //as well as both the isr funct and aux in the isrtab

    uart->opened = 0; //marks the device as closed
}


int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    struct uart_serial * uart = 
        (void *)ser - offsetof(struct uart_serial, base);
    lock_acquire(&uart->rq_lock);
    if (!uart->opened) {
        lock_release(&uart->rq_lock);
        return -EINVAL; //if the serial hasn't been opened yet we return an error. 
    }

    
    //if (bufsz <1) return -ENOTSUP; //the bufsz shouldn't be less than the blksz
    
    if (bufsz < 1) {
        lock_release(&uart->rq_lock);
        return 0;//Fix to test_uart_recv_none!!!
    }


    //if the ring buffer is empty we spin wait until it has some data.
    int pie = disable_interrupts();
    while (rbuf_empty(&uart->rxbuf)) condition_wait(&uart->rxbnotempty);
    restore_interrupts(pie);
    

    //while the ring buffer isn't empty and until we've recieved bufsz Bytes, we recieve bytes from the ring buffer.
    unsigned int numread = 0;
    while (!rbuf_empty(&uart->rxbuf) && numread < bufsz){

      ((char *)buf)[numread++] = rbuf_getc(&uart->rxbuf);

      uart->regs->ier |= IER_DRIE;
    }

    lock_release(&uart->rq_lock);
    return numread; //returns the number of bytes recieved from the ring buffer.
}



int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz) {
    
  struct uart_serial * uart = (void *)ser - offsetof(struct uart_serial, base);
    lock_acquire(&uart->rq_lock);
  if (!uart->opened) {
    lock_release(&uart->rq_lock);
    return -EINVAL;
  }

  unsigned int nwritten = 0;

  if (bufsz < 1) {
    lock_release(&uart->rq_lock);
    return nwritten; //we return 0 bytes sent if the bufsz is less than the blksz 
  }

  //until we have sent bufsz bytes to the ring buffer, we continue to send bytes to the ring buffer.
  //if the ring buffer is ever empty, we spin wait until it has some data in it
  while (nwritten < bufsz){

    int pie = disable_interrupts();
    while (rbuf_full(&uart->txbuf)) condition_wait(&uart->txbnotfull);
    restore_interrupts(pie);


    if (!rbuf_full(&uart->txbuf)) {
      rbuf_putc(&uart->txbuf, ((char *) buf)[nwritten++]);
    }

      uart->regs->ier |= IER_THREIE;
  }
  lock_release(&uart->rq_lock);
  return nwritten; //returns the number of bytes sent to the ring buffer
}

void uart_isr(int srcno, void * aux) {
    
    //type cast uart as a more convenient form of the aux.
    struct uart_serial * uart = (struct uart_serial *) aux;
   
    //if we read the "data ready" status from the LSR, we service the interrupt by reading from the RBR into the recieve buffer
    if (uart->regs->lsr & LSR_DR){
      if (!rbuf_full(&uart->rxbuf)){
        rbuf_putc(&uart->rxbuf, uart->regs->rbr); //write the rbr char to the recieve buffer if not full
        condition_broadcast(&uart->rxbnotempty);
      }
      else {
        //uart->rxovrcnt++;
        uart->regs->ier &= ~IER_DRIE; //disable data ready interrupt if rxbuf is full
      }
    }


    //if we read the "transmit holding empty" status from the LSR, we service the interrupt by writing to the THR with a character from the transmit buffer
    if (uart->regs->lsr & LSR_THRE){
      if (!rbuf_empty(&uart->txbuf)){
        uart->regs->thr = rbuf_getc(&uart->txbuf); //write to the thr if the transmit buffer isn't empty.
        condition_broadcast(&uart->txbnotfull);
      }
      else uart->regs->ier &= ~IER_THREIE; //disable transmit holding empty interrupt if txbuf is empty
    }
}

void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}



int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}


int rbuf_full(const struct ringbuf * rbuf) {
    return (rbuf->tpos - rbuf->hpos == UART_RBUFSZ);
}


void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}
