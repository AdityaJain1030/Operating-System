// uart.c -  NS8250-compatible serial port
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
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


    // Initialize hardware

    uart->regs->ier = 0;
    uart->regs->lcr = LCR_DLAB;
    // fence o,o ?
    uart->regs->dll = 0x01;
    uart->regs->dlm = 0x00;
    // fence o,o ?
    uart->regs->lcr = 0; // DLAB=0

    serial_init(&uart->base, &uart_serial_intf);        // initalizes interfaces
    register_device(UART_DEVNAME, DEV_SERIAL, uart);    // register device with system so it can be found
}
/* int uart_serial_open(struct serial *ser)
 * Inputs:
 *    struct serial *ser - Pointer to the serial structure representing the UART device
 * Outputs: None
 * Return Value:
 *    0       - Success
 *   -EBUSY   - The UART serial interface is already open
 * Function: Opens the specified UART serial interface for communication. Initializes
 *            the receive and transmit buffers, clears any stale data by reading
 *            from the RBR register, enables the data-ready interrupt, and registers
 *            the UART interrupt handler. Marks the serial interface as opened using
 *            the 'opened' flag to prevent duplicate opens.
 * Side Effects:
 *    - Modifies UART device registers.
 *    - May configure PLIC to enable interrupts for device
 *    - Updates the serial structure state (e.g., buffers and 'opened' flag).
 */
int uart_serial_open(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (uart->opened)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // Enable interrupts when data ready (DR) status asserted

    uart->opened = 1;
    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart);    // enable interrupt source
    uart->regs->ier |= IER_DRIE; // enable DR interrupt

    return 0;
}
/* void uart_serial_close(struct serial *ser)
 * Inputs:
 *    struct serial *ser - Pointer to the serial structure representing the UART device
 * Outputs: None
 * Return Value: None
 * Function: Closes the specified UART. Disables UART communication
 *            by disabling all device interrupts, unregistering the device as needed,
 *            and marking the serial interface as closed using the 'opened' flag.
 *            If the serial interface is not currently open, the function performs no action.
 * Side Effects:
 *    - Disables UART hardware interrupts by configuring the PLIC
 *    - Modifies the serial structure state (e.g., 'opened' flag).
 */
void uart_serial_close(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (!uart->opened)
        return;
    disable_intr_source(uart->irqno);
    uart->regs->ier = 0;     // disable all interrupts
    uart->opened = 0;
}

/* int uart_serial_recv(struct serial *ser, void *buf, unsigned int bufsz)
 * Inputs:
 *    struct serial *ser  - Pointer to the UART serial interface structure
 *    void *buf           - Pointer to the buffer to store received data
 *    unsigned int bufsz  - Maximum number of bytes to read from the UART
 * Outputs:
 *    buf - Filled with up to bufsz bytes of received data
 * Return Value:
 *    >= 0      - Number of bytes successfully read
 *    -EINVAL   - The specified UART serial interface is not currently open
 * Function: Reads data from the UART receive ring buffer and copies it into the 
 *            provided buffer. The number of bytes read will be between 1 and 
 *            bufsz, inclusive. If the buffer is empty, the thread will condition wait until rxbuf is not empty
 * Side Effects:
 *    - May suspend the calling thread if no data is available
 *    - Modifies the receive ring buffer and potentially the serial structure state.
 *    - Enables UART data-ready interrupts (modifying the IER register of UART)
 */
int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    
     struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);
    if (!uart->opened) return -EINVAL;

    char* dstbuf = (char*) buf;
    
    int i = 0;
    // Critical Section: Interrupt could occur between checking if rbuf_empty and condition_waiting
    // Don't want to condition_wait on a condition that already occured
    int pie = disable_interrupts();
    for (; i < bufsz; i++) {
        while (rbuf_empty(&uart->rxbuf))
        {
            condition_wait(&(uart->rxbnotempty));
        }
        dstbuf[i] = rbuf_getc(&uart->rxbuf);
        uart->regs->ier |= IER_DRIE;                    // ready to read in another byte
    }
    restore_interrupts(pie);
    return bufsz;
}


/* int uart_serial_send(struct serial *ser, const void *buf, unsigned int bufsz)
 * Inputs:
 *    struct serial *ser  - Pointer to the UART serial interface structure
 *    const void *buf     - Pointer to the buffer containing data to send
 *    unsigned int bufsz  - Number of bytes to write to the UART
 * Outputs: None
 * Return Value:
 *    >= 0      - Number of bytes successfully written to the transmit buffer
 *    -EINVAL   - The specified UART serial interface is not currently open
 * Function: Sends data from the provided buffer to the UART transmit ring buffer.
 *            Writes characters continuously until all bufsz bytes are copied. If
 *            the transmit buffer is full, condition_waits until transmit buffer is not full
 * Side Effects:
 *    - May suspend the calling thread if the transmit buffer is full
 *    - Modifies the transmit ring buffer and potentially the serial structure state.
 *    - Enables UART transmit interrupts by modifying IER register of UART
 */
int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz) {
     struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

        if (!uart->opened) return -EINVAL;
        char* dstbuf = (char*) buf;
        int i = 0;
        for (; i < bufsz; i++) {
            // MUST have a uart->regs->ier in the WHILE loop as software writing to buffer is MUCH faster than hardware reading from the buffer and transmitting
            // For example, if we only assert above/right here, its possible that LSR_THRE is never set because the THR is still full from the previous byte, and we never get to write to the buffer
            // Thus, we will be stuck in the WHILE loop forever, since we no longer check afterwards!
            
            // Critical Section: Interrupt could occur between while and condition_wait
            int pie = disable_interrupts();
            while (rbuf_full(&uart->txbuf)) {
                condition_wait(&(uart->txbnotfull));
            }
            restore_interrupts(pie);
            rbuf_putc(&uart->txbuf, dstbuf[i]);
            uart->regs->ier |= IER_THREIE; // enable THRE interrupt
        }
        // enable THRE interrupt
        // DO NOT disable THRE interrupt here, it is disabled in the ISR when the txbuf is empty
    return bufsz;
}
/* void uart_isr(int srcno, void *aux)
 * Inputs:
 *    int srcno  - Source number of the interrupt (IRQ) corresponding to the UART device
 *    void *aux  - Auxiliary data passed to the ISR, typically the UART serial structure
 * Outputs: None
 * Return Value: None
 * Function: Interrupt Service Routine (ISR) for the UART device. Handles hardware-triggered
 *            UART interrupts. The ISR checks the status registers and writes/reads into the corresponding
 *            RXBuf/TXBuf accordingly. Disables corresponding interrupts when there is no need to service them
 *            based upon certain conditions
 * Side Effects:
 *    - Modifies receive and transmit buffers
 *    - May enable or disable UART interrupts modifying UART registers
 *    - May wake threads waiting on condition variables
 */
void uart_isr(int srcno, void * aux) {
    struct uart_serial * uart = (struct uart_serial *)aux;

    // NEED TO KNOW WHY THE INTERRUPT HAPPENED HENCE CHECK THE BITS
    /*

    Summer 2025 lecture says to use IIR (interrupt identifcactionr register...)

    If Overrun need to READ the line status register! (at least thats what the spec says, so maybe use 110)

    However it seems ECE391 FA25 (Current class) does not implement this



    */

    // Checks corresponding status bits to see if data needs to be placed into buffers
    if (uart->regs->lsr & LSR_DR) { // if data ready
        if (!rbuf_full(&uart->rxbuf)) {
            rbuf_putc(&uart->rxbuf, uart->regs->rbr);   // read from RBR
            condition_broadcast(&(uart->rxbnotempty));
        } else {
            uart->rxovrcnt++;
        }
    }
    if (uart->regs->lsr & LSR_THRE) { // if THR empty
        if (!rbuf_empty(&uart->txbuf)) {
            char c = rbuf_getc(&uart->txbuf);
            uart->regs->thr = c; // write to THR
            condition_broadcast(&(uart->txbnotfull));       // wake up threads waiting on condition
        }
    }



    // Disable THRE interrupt. Nothing to write THR to
    if (rbuf_empty(&uart->txbuf)) {
        uart->regs->ier &= ~IER_THREIE; // disable THRE interrupt
    }
    // Disable DR interrupt. No space in buffer to read from RBR
    if (rbuf_full(&uart->rxbuf)) {
        uart->regs->ier &= ~IER_DRIE; // disable DR interrupt
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