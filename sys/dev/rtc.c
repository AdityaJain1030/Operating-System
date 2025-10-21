// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "rtc.h"
#include "conf.h"
#include "assert.h"
#include "devimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low;  // read first, latches time_high
    uint32_t time_high; //
};

struct rtc_device {
    struct serial base; // must be first
    volatile struct rtc_regs * regs;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct serial * ser);
static void rtc_close(struct serial * ser);
static int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

static const struct serial_intf rtc_serial_intf = {
    .blksz = 8,
    .open = &rtc_open,
    .close = &rtc_close,
    .recv = &rtc_recv
};

// EXPORTED FUNCTION DEFINITIONS
// 

/* void rtc_attach(void *mmio_base)
 * Inputs:
 *    void *mmio_base - Base address of the memory-mapped RTC registers
 * Outputs: None
 * Return Value: None
 * Function: Registers the RTC device with the system and initializes its serial
 *            interface and memory-mapped registers.
 * Side Effects: Modifies device registration state and RTC MMIO registers
 */
void rtc_attach(void * mmio_base) {
    // FIXME your code goes here
    struct rtc_device * rtc_dev;
    rtc_dev = kcalloc(1, sizeof(struct rtc_device));
    rtc_dev->regs = (struct rtc_regs *) mmio_base;
    serial_init(&rtc_dev->base, &rtc_serial_intf);
    register_device("rtc", DEV_SERIAL, rtc_dev);
}

int rtc_open(struct serial * ser) {
    trace("%s()", __func__);
    return 0;
}

void rtc_close(struct serial * ser) {
    trace("%s()", __func__);
}
/* int rtc_recv(struct serial *ser, void *buf, unsigned int bufsz)
 * Inputs:
 *    struct serial *ser  - Pointer to the RTC device serial structure
 *    void *buf           - Buffer to store the current time
 *    unsigned int bufsz  - Size of the buffer in bytes
 * Outputs:
 *    buf - Filled with the current RTC timestamp
 * Return Value: Number of bytes written to buf
 * Function: Reads the current real-time clock value and writes it into the
 *            provided buffer
 * Side Effects: Reads device registers and modifies buf
 */
int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    // NEED to check if bufsz == 0!!!
    if (ser == NULL || buf == NULL || bufsz == 0)
        return 0;
    struct rtc_device * const rtc =
        (void*)ser - offsetof(struct rtc_device, base);

    // reads from RTC register and stores time in provided buf
    uint64_t t = read_real_time(rtc->regs);
    uint64_t* data_addr = (uint64_t*) buf;
    *data_addr = t;
    return 8;
}
/* uint64_t read_real_time(volatile struct rtc_regs *regs)
 * Inputs:
 *    volatile struct rtc_regs *regs - Pointer to memory-mapped RTC registers
 * Outputs: None
 * Return Value: 64-bit timestamp read from RTC registers
 * Function: Reads and returns the full 64-bit current time from the RTC registers
 * Side Effects: Reads device registers
 */
uint64_t read_real_time(volatile struct rtc_regs * regs) {
    // FIXME your code goes here
    return regs->time_low + ((uint64_t)regs->time_high << 32);  // read from the low first as low can change while high takes longer to change
}