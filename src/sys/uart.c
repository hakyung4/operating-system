// uart.c - NS8250-compatible uart port
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
#include "device.h"
#include "intr.h"
#include "heap.h"
#include "thread.h"

#include "ioimpl.h"
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

#ifndef UART_NAME
#define UART_NAME "uart"
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

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

struct uart_device {
    volatile struct uart_regs * regs;
    int irqno;
    int instno;

    struct io io;

    unsigned long rxovrcnt; // number of times OE was set

    struct ringbuf rxbuf;
    struct ringbuf txbuf;
    struct lock uart_lock;
};

struct condition rxbuf_not_empty; // setup new conditions in place of spinning
struct condition txbuf_not_full;

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_open(struct io ** ioptr, void * aux);
static void uart_close(struct io * io);
static long uart_read(struct io * io, void * buf, long bufsz);
static long uart_write(struct io * io, const void * buf, long len);

static void uart_isr(int srcno, void * driver_private);

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// EXPORTED FUNCTION DEFINITIONS
// 

void uart_attach(void * mmio_base, int irqno) {
    static const struct iointf uart_iointf = {
        .close = &uart_close,
        .read = &uart_read,
        .write = &uart_write
    };

    struct uart_device * uart;

    uart = kcalloc(1, sizeof(struct uart_device));

    uart->regs = mmio_base;
    uart->irqno = irqno;

    ioinit0(&uart->io, &uart_iointf);

    // Check if we're trying to attach UART0, which is used for the console. It
    // had already been initialized and should not be accessed as a normal
    // device.

    if (mmio_base != (void*)UART0_MMIO_BASE) {

        uart->regs->ier = 0;
        uart->regs->lcr = LCR_DLAB;
        // fence o,o ?
        uart->regs->dll = 0x01;
        uart->regs->dlm = 0x00;
        // fence o,o ?
        uart->regs->lcr = 0; // DLAB=0

        uart->instno = register_device(UART_NAME, uart_open, uart);

    } else
        uart->instno = register_device(UART_NAME, NULL, NULL);

    condition_init(&rxbuf_not_empty, "rxbuf"); // intialize the conditions
    condition_init(&txbuf_not_full, "txbuf");

    lock_init(&uart->uart_lock);
}

// int uart_open(struct io ** ioptr, void * aux)
// Inputs: 
//   struct io ** ioptr - Pointer to store the opened UART device interface.
//   void * aux - Auxiliary data containing the UART device instance.
// Outputs: 
//   int - Returns 0 on success, or -EBUSY if the UART device is already in use.
// Description: 
//   Opens the UART device, initializes its receive and transmit buffers, 
//   enables data-ready interrupts, and registers an interrupt handler.
// Side Effects: 
//   Modifies the UART interrupt enable register and reference count.

int uart_open(struct io ** ioptr, void * aux) {
    struct uart_device * const uart = aux;

    trace("%s()", __func__);

    if (iorefcnt(&uart->io) != 0)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    uart->regs->ier |= IER_DRIE; // enable data ready interrupt

    //register interrupt handler

    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart); //register interrupt handler
    enable_interrupts();
    // FIXME your code goes here

    *ioptr = &uart->io; // give io pointer a reference to the uart device io

    uart->io.refcnt++; // increment reference count for every time its opened

    return 0;
}


// void uart_close(struct io * io)
// Inputs: 
//   struct io * io - Pointer to the UART device interface to be closed.
// Outputs: 
//   None
// Description: 
//   Closes the UART device by disabling interrupts and unregistering the interrupt handler.
// Side Effects: 
//   Clears the UART interrupt enable register and unregisters the interrupt.

void uart_close(struct io * io) {
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);


    trace("%s()", __func__);
    assert (iorefcnt(io) == 0);

    uart->regs->ier = 0; // clear all interrupts

    disable_intr_source(uart->irqno); // unregister interrupt handler

    
    return;


    // FIXME your code goes here

}

// long uart_read(struct io * io, void * buf, long bufsz)
// Inputs: 
//   struct io * io - Pointer to the UART device interface.
//   void * buf - Buffer to store the received data.
//   long bufsz - Maximum number of bytes to read.
// Outputs: 
//   long - Number of bytes actually read, or 0 if no data is available, or -1 on error.
// Description: 
//   Reads available bytes from the UART receive buffer into the provided buffer.
//   The function waits if no data is available until at least one byte is received.
// Side Effects: 
//   Modifies the UART receive buffer and may block execution while waiting for data.


long uart_read(struct io * io, void * buf, long bufsz) {
    int pie;

    struct uart_device * const uart = (void*)io - offsetof(struct uart_device, io);

    char c;

    int loop_counter = 0;

    if(io == NULL || buf == NULL || bufsz < 0){
        panic("improper arguments for uart_read");
    }
    if(bufsz == 0){
        return 0; // if empty buffer there is nothing to read
    }

    lock_acquire(&uart->uart_lock);
    
    pie = disable_interrupts(); // make sure to always disable interrupts before condition wait
    if(rbuf_empty(&uart->rxbuf)){
        condition_wait(&rxbuf_not_empty); // instead of spin waiting we define a new condition to wait on
    }
    restore_interrupts(pie);
    // long prev_state = disable_interrupts();

    // uart->regs->ier &= ~IER_DRIE;


    int available = (int)(uart->rxbuf.tpos - uart->rxbuf.hpos); // how many bytes are available to read 
    
    if (available < bufsz) {
        loop_counter = available; // making sure it does not exceed buffer size
    } else {
        loop_counter = bufsz;
    }
    

    if (loop_counter > UART_RBUFSZ) {
        loop_counter = UART_RBUFSZ; // making sure it does not exceed uart read buffer
    }

    for(int i = 0; i < loop_counter; i++){
        c = rbuf_getc(&uart->rxbuf); // loop through recieve buffer to collect all the characters and copy it into the given void buffer
        ((char*)buf)[i] = c;
    }

    
    // restore_interrupts(prev_state);

    uart->regs->ier |= IER_DRIE; // restore the interrupts

    lock_release(&uart->uart_lock);

    return loop_counter; // how many bytes were read
    
    // FIXME your code goes here

}

// long uart_write(struct io * io, const void * buf, long len)
// Inputs: 
//   struct io * io - Pointer to the UART device interface.
//   const void * buf - Buffer containing the data to be written.
//   long len - Number of bytes to write.
// Outputs: 
//   long - Number of bytes actually written, or -1 on error.
// Description: 
//   Writes data to the UART transmit buffer and enables the transmit interrupt.
//   The function waits if the transmit buffer is full until space becomes available.
// Side Effects: 
//   Modifies the UART transmit buffer and enables the transmit interrupt.

long uart_write(struct io * io, const void * buf, long len) {
    int pie;
    struct uart_device * const uart = (void*)io - offsetof(struct uart_device, io);

    char c;
    
    if(io == NULL || buf == NULL || len < 0){
        panic("improper arguments for uart_read");
    }
    if(len == 0){
        return 0; // if nothing to write return 0
    }

    lock_acquire(&uart->uart_lock);

 
    for(int i = 0; i < len; i++){

        pie = disable_interrupts();
        if(rbuf_full(&uart->txbuf)){
            condition_wait(&txbuf_not_full); // wait until txbuf not being full is met (this will be tracked in isr)
        }
        restore_interrupts(pie);

        // long prev_state = disable_interrupts();

        // uart->regs->ier &= ~IER_THREIE;

        c = ((char*)buf)[i]; // get character from the void buffer

        rbuf_putc(&uart->txbuf, c); // put this character into the transmit buffer

        // restore_interrupts(prev_state);


        uart->regs->ier |= IER_THREIE; // enable interrupts since isr will not take care of it


    }

    lock_release(&uart->uart_lock);


    return len; // return how many bytes were written

    // FIXME your code goes here

}

// void uart_isr(int srcno, void * aux)
// Inputs: 
//   int srcno - Interrupt source number.
//   void * aux - Pointer to the UART device structure.
// Outputs: 
//   None
// Description: 
//   UART interrupt handler that processes both receive and transmit events.
//   If data is received, it is stored in the receive buffer. If the transmit buffer 
//   is not empty, it sends the next byte.
// Side Effects: 
//   Modifies the UART receive and transmit buffers. May disable interrupts if buffers are full.

void uart_isr(int srcno, void * aux) {
    struct uart_device * const uart = aux;

    uint8_t lsr = uart->regs->lsr;

    char read_data;
    char write_data;
    int prev_state = -2; // make sure it is an invalid entry to begin with (any negative number)

    if(lsr & LSR_DR){ // ensure data ready
        while(lsr & LSR_DR){
            read_data = uart->regs->rbr; // get read data

            if (lsr & LSR_OE) {
                uart->rxovrcnt++;  // Track how many times OE happens
            }

            if(!rbuf_full(&uart->rxbuf)){
                rbuf_putc(&uart->rxbuf, read_data);
                condition_broadcast(&rxbuf_not_empty); // since we know after putc that rbuf cannot be empty we broadcast
            }
            else if(rbuf_full(&uart->rxbuf)){
                uart->regs->ier &= ~IER_DRIE;
            }

            lsr = uart->regs->lsr; // read again for more potential ready bytes
        }
    }
    if((lsr & LSR_THRE) && !rbuf_empty(&uart->txbuf)){
        while((lsr & LSR_THRE) && !rbuf_empty(&uart->txbuf)){
            write_data = rbuf_getc(&uart->txbuf);
            condition_broadcast(&txbuf_not_full); // since we know txbuf cannot be full after getting a char we can broadcast
            uart->regs->thr = write_data;

            lsr = uart->regs->lsr; // force another read in case there are more
        }
    }
    if(rbuf_empty(&uart->txbuf) && (lsr & LSR_THRE)){
        uart->regs->ier &= ~IER_THREIE; // if transmit buffer is empty, disable interrupt
    }

    // if (rbuf_empty(&uart->txbuf)) {
    //     uart->regs->ier &= ~IER_THREIE;  // Disable TX interrupt if no data left
    // }
    // if (rbuf_full(&uart->rxbuf)) {
    //     uart->regs->ier &= ~IER_DRIE;  // Disable RX interrupt if buffer is full
    // }

    if(prev_state != -2){
        restore_interrupts(prev_state); // if the prev_state was changed then we need to restore interrupts
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
    return ((uint16_t)(rbuf->tpos - rbuf->hpos) == UART_RBUFSZ);
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
