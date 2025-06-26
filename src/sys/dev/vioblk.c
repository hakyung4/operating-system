// vioblk.c - VirtIO serial port (console)
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include "virtio.h"
#include "intr.h"
#include "assert.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "thread.h"
#include "error.h"
#include "string.h"
#include "ioimpl.h"
#include "io.h"
#include "conf.h"

#include <limits.h>
#include <errno.h>

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define VIOBLK_VQ_LEN 8
#define VIRTIO_MMIO_INT_VRING   0x01  // Used ring notification
#define VIRTIO_MMIO_INT_CONFIG  0x02  // Config change notification
#define VIRTIO_BLK_T_IN 0 // type of request: read
#define VIRTIO_BLK_T_OUT 1 // type of request: write


// INTERNAL TYPE DEFINITIONS
//
struct vioblk_device {
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    int instno;

    struct io io;

    struct {
        uint16_t last_used_idx;

        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(VIOBLK_VQ_LEN)];
        };

        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(VIOBLK_VQ_LEN)];
        };

        struct virtq_desc desc[VIOBLK_VQ_LEN];
    } vq;


    uint32_t blksz;

    struct lock qlock;
    struct condition qwait;
};

// request structure tells the device what to do
struct virtio_blk_req {
    uint32_t type;       // Request type (IN, OUT, etc.)
    uint32_t reserved;   // Always 0
    uint64_t sector;     // Starting sector number
    // Followed by:
    //   - data buffer (read or write buffer)
    //   - 1-byte status field
};



// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX       1
#define VIRTIO_BLK_F_SEG_MAX        2
#define VIRTIO_BLK_F_GEOMETRY       4
#define VIRTIO_BLK_F_RO             5
#define VIRTIO_BLK_F_BLK_SIZE       6
#define VIRTIO_BLK_F_FLUSH          9
#define VIRTIO_BLK_F_TOPOLOGY       10
#define VIRTIO_BLK_F_CONFIG_WCE     11
#define VIRTIO_BLK_F_MQ             12
#define VIRTIO_BLK_F_DISCARD        13
#define VIRTIO_BLK_F_WRITE_ZEROES   14

// INTERNAL FUNCTION DECLARATIONS
//

static int vioblk_open(struct io ** ioptr, void * aux);
static void vioblk_close(struct io * io);

static long vioblk_readat (
    struct io * io,
    unsigned long long pos,
    void * buf,
    long bufsz);

static long vioblk_writeat (
    struct io * io,
    unsigned long long pos,
    const void * buf,
    long len);

static int vioblk_cntl (
    struct io * io, int cmd, void * arg);

static void vioblk_isr(int srcno, void * aux);

static int get_free_desc(struct vioblk_device *blkio);

// EXPORTED FUNCTION DEFINITIONS
//
// Attaches a VirtIO block device. Declared and called directly from virtio.c.

void vioblk_attach(volatile struct virtio_mmio_regs * regs, int irqno) {

    //set up io interface like how we did with uart
    static const struct iointf vioblk_iointf = {
        .close = &vioblk_close,
        .readat = &vioblk_readat,
        .writeat = &vioblk_writeat,
        .cntl = &vioblk_cntl
    }; 

    if (regs->device_id != VIRTIO_ID_BLOCK) {
        return;
    }

    // force the device reset
    regs->status = 0;

    // noticed the device
    regs->status |= VIRTIO_STAT_ACKNOWLEDGE;
    // Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;

    // fence o,io
    __sync_synchronize();

    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    // Mandatory features
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    // Optional features
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE); // give me block size  should be 512 bytes for our case
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);

    // if failed -> set fail bit and reset
    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        regs->status |= VIRTIO_STAT_FAILED;
        regs->status = 0;
        return;
    }

    // re-read device status to ensure the FEATURES_OK bit is still set
    if (!(regs->status & VIRTIO_STAT_FEATURES_OK)) {
        kprintf("%p:[Re-reading] virtio feature negotiation failed\n", regs);
        regs->status |= VIRTIO_STAT_FAILED;
        regs->status = 0;
        return;
    }
    //allocate space for the new device
    struct vioblk_device* blkio = kcalloc(1, sizeof(struct vioblk_device)); 

    if (!blkio) {
        return;
    }

    blkio->regs = regs;
    blkio->irqno = irqno;
    blkio->instno = register_device(VIOBLK_NAME, vioblk_open, blkio);



    //device-specific setup, including discovery of virtqueues for the device,

    // only using one vq
    regs->queue_sel = 0;
    __sync_synchronize(); // fence o,o

    // queue is already in use; expected to be 0
    if (regs->queue_ready != 0) {
        regs->status |= VIRTIO_STAT_FAILED;
        regs->status = 0;
        return;
    }

    // queue not available
    if (regs->queue_num_max == 0) {
        regs->status |= VIRTIO_STAT_FAILED;
        regs->status = 0;
        return;
    }

    // zero the vq memory:
    memset(&blkio->vq, 0, sizeof(blkio->vq));

    regs->queue_num = VIOBLK_VQ_LEN;
    regs->queue_ready = 1;


    virtio_attach_virtq(regs, 0, VIOBLK_VQ_LEN, (uint64_t)(uintptr_t)&blkio->vq.desc, (uint64_t)(uintptr_t)&blkio->vq.used, (uint64_t)(uintptr_t)&blkio->vq.avail);


    // If the device provides a block size, use it. Otherwise, use 512.
    uint32_t blksz;
    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    if (((blksz - 1) & blksz) != 0) {
        return;
    }
    
    blkio->blksz = blksz;

    condition_init(&blkio->qwait, "queuewait");
    lock_init(&blkio->qlock);


    ioinit0(&blkio->io, &vioblk_iointf);

    __sync_synchronize();
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();
 
    // create cache or I don't need to worry about cache here.


}

static void vioblk_close(struct io*	io) {
    struct vioblk_device* const blkio = (void*)io - offsetof(struct vioblk_device, io);

    if (iorefcnt(io) != 0) {
        return;
    }

    // reset the virtqueue
    virtio_reset_virtq(blkio->regs, 0);
    __sync_synchronize(); 
    // reset the device
    blkio->regs->status = 0;
    __sync_synchronize(); 
    // disable interrupts
    disable_intr_source(blkio->irqno);

    blkio->vq.avail.idx = 0;
    blkio->vq.used.idx  = 0;
    blkio->vq.last_used_idx = 0;

}	

static int vioblk_cntl (struct io * io, int cmd, void * arg) {
    struct vioblk_device* const blkio = (void*)io - offsetof(struct vioblk_device, io);

    
    switch (cmd) {
        case IOCTL_GETEND: {
            if (arg == NULL) {
                return -EINVAL;
            }
            unsigned long long capacity = blkio->regs->config.blk.capacity;
            *(unsigned long long*)arg = capacity * (unsigned long long)blkio->blksz;
            return 0;
        }
        case IOCTL_GETBLKSZ:
            if (arg == NULL) {
                return -EINVAL;
            }
            *(uint32_t*)arg = blkio->blksz;
            return 0;

        default:
            return -ENOTSUP;
    }
}

static int vioblk_open(struct io ** ioptr, void * aux) {

    if (ioptr == NULL || aux == NULL) {
        return -EINVAL;
    }

    struct vioblk_device* const blkio = aux;

    virtio_enable_virtq(blkio->regs, 0);
    __sync_synchronize(); 



    enable_intr_source(blkio->irqno, VIOBLK_INTR_PRIO, vioblk_isr, blkio);
    *ioptr = ioaddref(&blkio->io); 
    return 0;

}

static void vioblk_isr(int srcno, void * aux) {

    struct vioblk_device* const blkio = aux;

    uint32_t int_status = blkio->regs->interrupt_status; // find out the interrupt enabled bits

    // 3 types of notification
    
    // config and used buffer notification are sent by the device to the driver
    // config change
    // used buffer notification

    // sent by driver to device
    // avail buffer notification


        // used buffer notification
    if (int_status & VIRTIO_MMIO_INT_VRING) {
        // do something
        condition_broadcast(&blkio->qwait);

    }
    // configuration change notification
    if (int_status & VIRTIO_MMIO_INT_CONFIG) {
        // anything that's related to config
        panic("viorng: config change notification and shouldn't happen");
    }

    // notifies the device that events causing the interrupt have been handled.
    blkio->regs->interrupt_ack = int_status; 
    __sync_synchronize();
}

static long vioblk_readat(struct io *io, unsigned long long pos, void *buf, long bufsz) {
    struct vioblk_device* blkio = (void*)io - offsetof(struct vioblk_device, io);
    if (bufsz < 0 || bufsz % blkio->blksz != 0 || pos % blkio->blksz != 0)
        return -EINVAL;
    if (bufsz == 0)
        return 0;
    unsigned long long end = pos + bufsz;  // or `len` for writeat
    unsigned long long total = blkio->regs->config.blk.capacity * blkio->blksz;
    if (end > total) {
        return -EINVAL;
    }


    lock_acquire(&blkio->qlock);

    // maybe i need to check if pos + bufsz <= endpoint to prevent reading beyond the end of the device 

    // Allocate and set up the request and status buffer
    struct virtio_blk_req *req = kcalloc(1, sizeof(*req));
    uint8_t *status = kcalloc(1, sizeof(uint8_t));

    if (!req || !status) {
        lock_release(&blkio->qlock);
        return -ENOMEM;
    }

    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = pos / blkio->blksz;

    // Find 3 free descriptors dynamically
    int head = get_free_desc(blkio);
    if (head == -1) {
        lock_release(&blkio->qlock);
        kfree(req);
        kfree(status);
        return -ENOMEM;  // No free descriptors available
    }

    int data = (head + 1) % VIOBLK_VQ_LEN;
    int stat = (head + 2) % VIOBLK_VQ_LEN;

    // Setup descriptors
    blkio->vq.desc[head].addr = (uint64_t)(uintptr_t)req;
    blkio->vq.desc[head].len = sizeof(*req);
    blkio->vq.desc[head].flags = VIRTQ_DESC_F_NEXT;
    blkio->vq.desc[head].next = data;

    blkio->vq.desc[data].addr = (uint64_t)(uintptr_t)buf;
    blkio->vq.desc[data].len = bufsz;
    blkio->vq.desc[data].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
    blkio->vq.desc[data].next = stat;

    blkio->vq.desc[stat].addr = (uint64_t)(uintptr_t)status;
    blkio->vq.desc[stat].len = 1;
    blkio->vq.desc[stat].flags = VIRTQ_DESC_F_WRITE;

    // Submit the descriptors
    uint16_t idx = blkio->vq.avail.idx % VIOBLK_VQ_LEN;
    blkio->vq.avail.ring[idx] = head;
    __sync_synchronize();
    blkio->vq.avail.idx++;
    __sync_synchronize();

    virtio_notify_avail(blkio->regs, 0);

    // Wait for the completion of the request
    while (blkio->vq.used.idx == blkio->vq.last_used_idx)
        condition_wait(&blkio->qwait);

    blkio->vq.last_used_idx++;

    int ret = (status[0] == 0) ? bufsz : -EIO;

    blkio->vq.desc[head].flags = 0;
    blkio->vq.desc[data].flags = 0; 
    blkio->vq.desc[stat].flags = 0;

    lock_release(&blkio->qlock);
    kfree(req);
    kfree(status);
    return ret;
}

static long vioblk_writeat(struct io *io, unsigned long long pos, const void *buf, long len) {
    struct vioblk_device* blkio = (void*)io - offsetof(struct vioblk_device, io);

    if (len <= 0 || len % blkio->blksz != 0 || pos % blkio->blksz != 0)
        return -EINVAL;

    unsigned long long end = pos + len;
    unsigned long long total = blkio->regs->config.blk.capacity * blkio->blksz;

    // Check if the write goes beyond the end of the device
    if (end > total) {
        return -EINVAL;
    }

    lock_acquire(&blkio->qlock);

    // Allocate and set up the request and status buffer
    struct virtio_blk_req *req = kcalloc(1, sizeof(*req));
    uint8_t *status = kcalloc(1, sizeof(uint8_t));

    // A driver MUST NOT create a descriptor chain longer than the Queue Size of the device.


    if (!req || !status) {
        lock_release(&blkio->qlock);
        return -ENOMEM;
    }

    req->type = VIRTIO_BLK_T_OUT;
    req->reserved = 0;
    req->sector = pos / blkio->blksz;

    // Find 3 free descriptors dynamically
    int head = get_free_desc(blkio);
    if (head == -1) {
        lock_release(&blkio->qlock);
        kfree(req);
        kfree(status);
        return -ENOMEM;  // No free descriptors available
    }

    int data = (head + 1) % VIOBLK_VQ_LEN;
    int stat = (head + 2) % VIOBLK_VQ_LEN;

    // Setup descriptors
    blkio->vq.desc[head].addr = (uint64_t)(uintptr_t)req;
    blkio->vq.desc[head].len = sizeof(*req);
    blkio->vq.desc[head].flags = VIRTQ_DESC_F_NEXT;
    blkio->vq.desc[head].next = data;

    blkio->vq.desc[data].addr = (uint64_t)(uintptr_t)buf;
    blkio->vq.desc[data].len = len;
    blkio->vq.desc[data].flags = VIRTQ_DESC_F_NEXT;
    blkio->vq.desc[data].next = stat;

    blkio->vq.desc[stat].addr = (uint64_t)(uintptr_t)status;
    blkio->vq.desc[stat].len = 1;
    blkio->vq.desc[stat].flags = VIRTQ_DESC_F_WRITE;

    // Submit the descriptors
    uint16_t idx = blkio->vq.avail.idx % VIOBLK_VQ_LEN;
    blkio->vq.avail.ring[idx] = head;
    __sync_synchronize();
    blkio->vq.avail.idx++;
    __sync_synchronize();

    virtio_notify_avail(blkio->regs, 0);

    // Wait for the completion of the request
    while (blkio->vq.used.idx == blkio->vq.last_used_idx)
        condition_wait(&blkio->qwait);

    blkio->vq.last_used_idx++;

    int ret = (status[0] == 0) ? len : -EIO;

    blkio->vq.desc[head].flags = 0;
    blkio->vq.desc[data].flags = 0;
    blkio->vq.desc[stat].flags = 0;

    lock_release(&blkio->qlock);
    kfree(req);
    kfree(status);
    return ret;
}


// Looks for 3 free descriptors
static int get_free_desc(struct vioblk_device *blkio) {
    for (int i = 0; i < VIOBLK_VQ_LEN; i++) {
        int i1 = (i + 1) % VIOBLK_VQ_LEN;
        int i2 = (i + 2) % VIOBLK_VQ_LEN;
        if (blkio->vq.desc[i].flags == 0 &&
            blkio->vq.desc[i1].flags == 0 &&
            blkio->vq.desc[i2].flags == 0)
            return i;
    }
    return -1;
}