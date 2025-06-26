// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "rtc.h"
#include "device.h"
#include "ioimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low;
    uint32_t time_high;
};

struct rtc_device {
    volatile struct rtc_regs * regs;
    struct io io;
    int instno;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct io ** ioptr, void * aux);
static void rtc_close(struct io * io);
static int rtc_cntl(struct io * io, int cmd, void * arg);
static long rtc_read(struct io * io, void * buf, long bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// EXPORTED FUNCTION DEFINITIONS
// 

struct iointf rtc_intf = {
    .close = &rtc_close,
    .cntl = &rtc_cntl,
    .read = &rtc_read,
};

void rtc_attach(void * mmio_base) {
    struct rtc_device * rtc = kcalloc(1, sizeof(struct rtc_device)); // allocate memory

    rtc->io.intf = &rtc_intf; 

    rtc->io.refcnt = 0; // initialize refcount to 0 since there are no references until it is opened

    rtc->regs = mmio_base; //set base

    rtc->instno = register_device("rtc", rtc_open, rtc); // register rtc device
}


// int rtc_open(struct io ** ioptr, void * aux)
// Inputs: 
//   struct io ** ioptr - Pointer to store the opened RTC device interface.
//   void * aux - Auxiliary data containing the RTC device instance.
// Outputs: 
//   int - Returns 0 on success, or triggers a panic if arguments are invalid.
// Description: 
//   Opens an RTC device, initializes the device interface, and increments the reference count.
// Side Effects: 
//   Modifies the reference count of the RTC device.

int rtc_open(struct io ** ioptr, void * aux) {
    struct rtc_device * rtc = aux;
    if(ioptr == NULL || rtc == NULL){
        panic("Bad arguments for rtc_open");
    }
    *ioptr = &rtc->io; //give reference to the io of the device
    rtc->io.refcnt++; // increment how many references

    return 0;
}


// void rtc_close(struct io * io)
// Inputs: 
//   struct io * io - Pointer to the RTC device interface to be closed.
// Outputs: 
//   None
// Description: 
//   Closes an RTC device by asserting that the reference count is 0.
// Side Effects: 
//   Triggers a panic if the provided pointer is NULL or incorrect.

void rtc_close(struct io * io) {
    struct rtc_device * rtc = (void*)io - offsetof(struct rtc_device, io);
    if(io == NULL){
        panic("Bad arguments for rtc_close"); //check for proper arguments
    }
    assert(rtc->io.refcnt == 0); // ensure there are no more references prior to closing
    // kfree(rtc);
}


// int rtc_cntl(struct io * io, int cmd, void * arg)
// Inputs: 
//   struct io * io - Pointer to the RTC device interface.
//   int cmd - Command identifier for control operations.
//   void * arg - Pointer to additional command-specific data (if applicable).
// Outputs: 
//   int - Returns 8 if the command is IOCTL_GETBLKSZ, otherwise returns 0.
// Description: 
//   Handles RTC control commands, such as retrieving the block size.
// Side Effects: 
//   Triggers a panic if the provided pointer is NULL.


int rtc_cntl(struct io * io, int cmd, void * arg) {
    if(io == NULL){
        panic("Bad arguments for rtc_cntl");
    }
    if(cmd == IOCTL_GETBLKSZ){
        return 8; // returns # of bytes
    }

    return 0;
}

// long rtc_read(struct io * io, void * buf, long bufsz)
// Inputs: 
//   struct io * io - Pointer to the RTC device interface.
//   void * buf - Buffer to store the read time value.
//   long bufsz - Size of the buffer in bytes.
// Outputs: 
//   long - Returns 8 on success, or -EINVAL if the buffer is too small or NULL.
// Description: 
//   Reads the current real-time clock value and copies it into the provided buffer.
// Side Effects: 
//   Triggers a panic if the provided RTC device interface pointer is NULL.

long rtc_read(struct io * io, void * buf, long bufsz) {
    struct rtc_device * const rtc = (void*)io - offsetof(struct rtc_device, io);
    if(io == NULL){
        panic("Bad arguments for rtc_read");
    }

    if(bufsz < sizeof(uint64_t)){
        return -EINVAL;
    }

    if(buf == NULL){
        return -EINVAL;
    }

    uint64_t real_time = read_real_time(rtc->regs); // get real-time clock value

    memcpy(buf, &real_time, sizeof(real_time)); // copy mem into buffer

    return 8;
}

// uint64_t read_real_time(volatile struct rtc_regs * regs)
// Inputs: 
//   volatile struct rtc_regs * regs - Pointer to the RTC register structure.
// Outputs: 
//   uint64_t - The current real-time clock value as a 64-bit integer.
// Description: 
//   Reads the low and high 32-bit time values from the RTC registers and 
//   combines them into a single 64-bit timestamp.
// Side Effects: 
//   None

uint64_t read_real_time(volatile struct rtc_regs * regs) {
    uint32_t low, high;

    low = regs->time_low; // read lower register that holds time

    high = regs->time_high; // read higher register that holds time as well

    return ((uint64_t)high << 32) | low; // splice the two together to form the entire time signature
}