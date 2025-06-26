// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "dev/virtio.h"
#include "intr.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "error.h"
#include "string.h"
#include "ioimpl.h"
#include "assert.h"
#include "conf.h"
#include "intr.h"
#include "console.h"
#include "thread.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "rng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

struct viorng_device {
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    int instno;

    struct io io;

    struct {
        uint16_t last_used_idx;

        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(1)];
        };

        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(1)];
        };

        // The first descriptor is a regular descriptor and is the one used in
        // the avail and used rings.

        struct virtq_desc desc[1];
    } vq;

    // bufcnt is the number of bytes left in buffer. The usable bytes are
    // between buf+0 and buf+bufcnt. (We read from the end of the buffer.)

    unsigned int bufcnt;
    char buf[VIORNG_BUFSZ];
};

struct condition descriptor_filled;

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_open(struct io ** ioptr, void * aux);
static void viorng_close(struct io * io);
static long viorng_read(struct io * io, void * buf, long bufsz);
static void viorng_isr(int irqno, void * aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.

// void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno)
// Inputs: 
//   volatile struct virtio_mmio_regs * regs - Pointer to the VirtIO MMIO register structure for the device.
//   int irqno - Interrupt number assigned to the VirtIO RNG device.
// Outputs: 
//   None
// Description: 
//   Attaches a VirtIO RNG device by performing feature negotiation, initializing the VirtIO queue,
//   and setting up device status flags. Registers the device within the system.
// Side Effects: 
//   Allocates memory for the device structure and updates MMIO registers for device initialization.

void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    //           FIXME add additional declarations here if needed


    //set up io interface like how we did with uart
    static const struct iointf viorng_iointf = {
        .close = &viorng_close,
        .read = &viorng_read
    };

    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
    
    if (regs->device_id != VIRTIO_ID_RNG) {
        return;
    }

    // Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;

    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    //           FIXME Finish viorng initialization here! 
    
    struct viorng_device * attachment = kcalloc(1, sizeof(struct viorng_device)); //allocate space for the new device


    //intialize device fields 

    attachment->regs = regs;

    attachment->irqno = irqno;

    attachment->instno = register_device(VIORNG_NAME, viorng_open, attachment);

    attachment->vq.desc[0].addr = (uint64_t)(uintptr_t)&attachment->buf;

    attachment->vq.desc[0].len = VIORNG_BUFSZ;

    attachment->vq.desc[0].flags = VIRTQ_DESC_F_WRITE;

    attachment->vq.last_used_idx = 0;

    ioinit0(&attachment->io, &viorng_iointf);


    // attachment->vq.desc[0].next = VIRTQ_DESC_F_NEXT; prolly not necessary type shi

    virtio_attach_virtq(regs, 0, 1, (uint64_t)(uintptr_t)&attachment->vq.desc, (uint64_t)(uintptr_t)&attachment->vq.used, (uint64_t)(uintptr_t)&attachment->vq.avail);

    condition_init(&descriptor_filled, "bytesfilled");

    // fence o,oi
    regs->status |= VIRTIO_STAT_DRIVER_OK;    
    //           fence o,oi
    __sync_synchronize();
}

// int viorng_open(struct io ** ioptr, void * aux)
// Inputs: 
//   struct io ** ioptr - Pointer to store the opened device interface.
//   void * aux - Pointer to the VirtIO RNG device structure.
// Outputs: 
//   int - Returns 0 on success, or calls panic() on invalid arguments.
// Description: 
//   Opens the VirtIO RNG device, initializes its queue, enables interrupts, 
//   and makes the device ready for random number generation requests.
// Side Effects: 
//   Modifies the VirtIO queue and enables device interrupts.

int viorng_open(struct io ** ioptr, void * aux) {

    if(ioptr == NULL){
        panic("improper io pointer argument");
    }
    if(aux == NULL){
        panic("improper aux argument reference to struct");
    }
    struct viorng_device * const device = aux;

    virtio_enable_virtq(device->regs, 0);

    //update avail.idx and put descriptor in there

    device->vq.avail.ring[0] = 0;

    device->vq.avail.idx = 1;

    virtio_notify_avail(device->regs, 0);

    enable_intr_source(device->irqno, VIORNG_INTR_PRIO, viorng_isr, device);


    *ioptr = &device->io; // pass a reference for the io

    device->io.refcnt++;

    return 0;
}

// void viorng_close(struct io * io)
// Inputs: 
//   struct io * io - Pointer to the VirtIO RNG device interface to be closed.
// Outputs: 
//   None
// Description: 
//   Closes the VirtIO RNG device by disabling interrupts and resetting the VirtIO queue.
// Side Effects: 
//   Clears the VirtIO queue, disables interrupts, and marks the device as inactive.

void viorng_close(struct io * io) {
    if(io == NULL){
        panic("improper io argument in viorng_close");
    }

    struct viorng_device * const device = (void*)io - offsetof(struct viorng_device, io);

    virtio_reset_virtq(device->regs, 0); // needs to be done such that the next time we open we do not have old stuff

    device->regs->status = 0;

    disable_intr_source(device->irqno);

    device->vq.avail.idx = 0;
    device->vq.used.idx  = 0;
    device->vq.last_used_idx = 0;

}

// long viorng_read(struct io * io, void * buf, long bufsz)
// Inputs: 
//   struct io * io - Pointer to the VirtIO RNG device interface.
//   void * buf - Buffer to store the random data retrieved from the device.
//   long bufsz - Maximum number of bytes to read.
// Outputs: 
//   long - Number of bytes actually read, or calls panic() on invalid arguments.
// Description: 
//   Reads random data from the VirtIO RNG device into the provided buffer. 
//   If no data is available, the function waits until data is ready.
// Side Effects: 
//   Modifies the VirtIO queue and buffers. May block execution until data is available.


long viorng_read(struct io * io, void * buf, long bufsz) {
    int pie;
    if(io == NULL || buf == NULL || bufsz < 0){
        panic("improper arguments in viorng_read");
    }
    if(bufsz == 0){
        return 0;
    }

    struct viorng_device * const device = (void*)io - offsetof(struct viorng_device, io);


    uint32_t queue_size = 1; // i think its 1? ask in oh to make sure

    pie = disable_interrupts();
    if(device->vq.used.idx == device->vq.last_used_idx){
        condition_wait(&descriptor_filled); // we need to wait until used is one further along than last_used
    }
    restore_interrupts(pie);
    
    uint32_t id_used = device->vq.used.ring[device->vq.last_used_idx % queue_size].id; // not actually used
    (void)id_used;
    uint32_t length = device->vq.used.ring[device->vq.last_used_idx % queue_size].len; // to know how big of a chunk we have

    device->vq.last_used_idx++;

    long copy_number;

    if(length > bufsz){
        copy_number = bufsz;
    }
    else{
        copy_number = length;
    }

    memcpy(buf, device->buf, copy_number); // easy way to copy into buffer instead of using a for loop

    device->bufcnt = 0;

    device->vq.avail.ring[device->vq.avail.idx % queue_size] = 0;
    device->vq.avail.idx++;
    virtio_notify_avail(device->regs, 0);  // notify device we posted again


    return copy_number;
}

// void viorng_isr(int irqno, void * aux)
// Inputs: 
//   int irqno - Interrupt source number.
//   void * aux - Pointer to the VirtIO RNG device structure.
// Outputs: 
//   None
// Description: 
//   Interrupt handler for the VirtIO RNG device that processes the interrupt, acknowledges it, 
//   and wakes up any waiting threads that requested random data.
// Side Effects: 
//   Modifies the VirtIO queue and may enable waiting threads to resume execution.

void viorng_isr(int irqno, void * aux) {
    //FIXME your code here

    struct viorng_device * const device = aux;

    uint32_t int_status = device->regs->interrupt_status; // find out the interrupt enabled bits

    device->regs->interrupt_ack |= int_status; // write them to the ack to say we are handling it
    
    condition_broadcast(&descriptor_filled); // since we are processing an interrupt that means we are ready to process more

    device->bufcnt = 256;

}   