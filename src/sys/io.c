// io.c - Unified I/O object
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "io.h"
#include "ioimpl.h"
#include "assert.h"
#include "string.h"
#include "heap.h"
#include "error.h"
#include "thread.h"
#include "memory.h"

#include <stddef.h>
#include <limits.h>
// INTERNAL TYPE DEFINITIONS
//

struct memio {
    struct io io; // I/O struct of memory I/O
    void * buf; // Block of memory
    size_t size; // Size of memory block
};

#define PIPE_BUFSZ PAGE_SIZE

struct seekio {
    struct io io; // I/O struct of seek I/O
    struct io * bkgio; // Backing I/O supporting _readat_ and _writeat_
    unsigned long long pos; // Current position within backing endpoint
    unsigned long long end; // End position in backing endpoint
    int blksz; // Block size of backing endpoint
};

struct pipe {
    struct io wio; // write I/O
    struct io rio; // read I/O
    void * buf; // Buffer
    size_t head, tail; // Head and tail positions
    int refcnt_w, refcnt_r; // Reference counts
    struct lock lock;
    struct condition can_read;
    struct condition can_write;
};

// INTERNAL FUNCTION DEFINITIONS
//

int memio_cntl(struct io * io, int cmd, void * arg);

long memio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz);

long memio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len);

void create_pipe(struct io ** wioptr, struct io ** rioptr);

static void seekio_close(struct io * io);

static int seekio_cntl(struct io * io, int cmd, void * arg);

static long seekio_read(struct io * io, void * buf, long bufsz);

static long seekio_write(struct io * io, const void * buf, long len);

static long seekio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz);

static long seekio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len);

static void pipe_close(struct io *io);
static long pipe_read(struct io *io, void *buf, long bufsz);
static long pipe_write(struct io *io, const void *buf, long len);
    
static const struct iointf pipe_w_intf = {
    .close = &pipe_close,
    .write = &pipe_write,
};

static const struct iointf pipe_r_intf = {
    .close = &pipe_close,
    .read = &pipe_read,
};
// INTERNAL GLOBAL CONSTANTS
static const struct iointf seekio_iointf = {
    .close = &seekio_close,
    .cntl = &seekio_cntl,
    .read = &seekio_read,
    .write = &seekio_write,
    .readat = &seekio_readat,
    .writeat = &seekio_writeat
};

static const struct iointf memio_iointf = {
    .cntl = &memio_cntl,
    .readat = &memio_readat,
    .writeat = &memio_writeat
};

// EXPORTED FUNCTION DEFINITIONS
//

struct io * ioinit0(struct io * io, const struct iointf * intf) {
    assert (io != NULL);
    assert (intf != NULL);
    io->intf = intf;
    io->refcnt = 0;
    return io;
}

struct io * ioinit1(struct io * io, const struct iointf * intf) {
    assert (io != NULL);
    io->intf = intf;
    io->refcnt = 1;
    return io;
}

unsigned long iorefcnt(const struct io * io) {
    assert (io != NULL);
    return io->refcnt;
}

struct io * ioaddref(struct io * io) {
    assert (io != NULL);
    io->refcnt += 1;
    return io;
}

void ioclose(struct io * io) {
    assert (io != NULL);
    assert (io->intf != NULL);
    
    assert (io->refcnt != 0);
    io->refcnt -= 1;

    if (io->refcnt == 0 && io->intf->close != NULL)
        io->intf->close(io);
}

long ioread(struct io * io, void * buf, long bufsz) {
    assert (io != NULL);
    assert (io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->read(io, buf, bufsz);
}

long iofill(struct io * io, void * buf, long bufsz) {
	long bufpos = 0; // position in buffer for next read
    long nread; // result of last read

    assert (io != NULL);
    assert (io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    while (bufpos < bufsz) {
        nread = io->intf->read(io, buf+bufpos, bufsz-bufpos);
        
        if (nread <= 0)
            return (nread < 0) ? nread : bufpos;
        
        bufpos += nread;
    }

    return bufpos;
}

long iowrite(struct io * io, const void * buf, long len) {
	long bufpos = 0; // position in buffer for next write
    long n; // result of last write

    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->write == NULL)
        return -ENOTSUP;

    if (len < 0)
        return -EINVAL;
    
    do {
        n = io->intf->write(io, buf+bufpos, len-bufpos);

        if (n <= 0)
            return (n < 0) ? n : bufpos;

        bufpos += n;
    } while (bufpos < len);

    return bufpos;
}

long ioreadat (
    struct io * io, unsigned long long pos, void * buf, long bufsz)
{
    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->readat == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->readat(io, pos, buf, bufsz);
}

long iowriteat (
    struct io * io, unsigned long long pos, const void * buf, long len)
{
    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->writeat == NULL)
        return -ENOTSUP;
    
    if (len < 0)
        return -EINVAL;
    
    return io->intf->writeat(io, pos, buf, len);
}

int ioctl(struct io * io, int cmd, void * arg) {
    assert (io != NULL);
    assert (io->intf != NULL);

	if (io->intf->cntl != NULL)
        return io->intf->cntl(io, cmd, arg);
    else if (cmd == IOCTL_GETBLKSZ)
        return 1; // default block size
    else
        return -ENOTSUP;
}

int ioblksz(struct io * io) {
    return ioctl(io, IOCTL_GETBLKSZ, NULL);
}

int ioseek(struct io * io, unsigned long long pos) {
    return ioctl(io, IOCTL_SETPOS, &pos);
}

struct io * create_memory_io(void * buf, size_t size) {
    // FIX ME
    if (buf == NULL || size == 0) {
        return NULL;
    }
    struct memio * memio = kcalloc(1, sizeof(struct memio));
    if (memio == NULL) {
        return NULL;
    }
    memio->buf = buf;
    memio->size = size;
    
    return ioinit1(&memio->io, &memio_iointf);
}

struct io * create_seekable_io(struct io * io) {
    struct seekio * sio;
    unsigned long end;
    int result;
    int blksz;
    
    blksz = ioblksz(io);
    assert (0 < blksz);
    
    // block size must be power of two
    assert ((blksz & (blksz - 1)) == 0);

    result = ioctl(io, IOCTL_GETEND, &end);
    assert (result == 0);
    
    sio = kcalloc(1, sizeof(struct seekio));

    sio->pos = 0;
    sio->end = end;
    sio->blksz = blksz;
    sio->bkgio = ioaddref(io);

    return ioinit1(&sio->io, &seekio_iointf);

};

// INTERNAL FUNCTION DEFINITIONS
//

long memio_readat (
    struct io * io,
    unsigned long long pos,
    void * buf, long bufsz)
{
    // FIX ME
    if (io == NULL || buf == NULL || bufsz < 0 || pos < 0) {
        return -EINVAL;
    }
    if (io->intf->readat == NULL) {
        return -ENOTSUP;
    }
    if (bufsz == 0) {
        return 0;
    }
    struct memio * memio = (struct memio *) io;
    if (pos >= memio->size) {
        return -EINVAL;
    }
    long max_read_size = memio->size - pos;
    long number_of_bytes_read = (bufsz > max_read_size) ? max_read_size : bufsz;
    memcpy(buf, (void * )((char *)memio->buf + pos), number_of_bytes_read);
    
    return number_of_bytes_read;
}

long memio_writeat (
    struct io * io,
    unsigned long long pos,
    const void * buf, long len)
{
    // FIX ME
    if (io->intf->writeat == NULL) {
        return -ENOTSUP;
    }
    if (io == NULL || buf == NULL || len < 0 || pos < 0) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    struct memio * memio = (struct memio *) io;
    if (pos >= memio->size) {
        return -EINVAL;
    }
    long max_write_size = memio->size - pos;
    long number_of_bytes_written = (len > max_write_size) ? max_write_size : len;
    memcpy((char *)memio->buf + pos, buf, number_of_bytes_written);

    return number_of_bytes_written;
}

int memio_cntl(struct io * io, int cmd, void * arg) {
    // FIX ME
    if (io == NULL) {
        return -EINVAL;
    }
    struct memio * memio = (struct memio *) io;
    switch (cmd) {
        case IOCTL_GETBLKSZ:
            if (arg == NULL) {
                return -EINVAL;
            }
            return 1;
        case IOCTL_GETEND:
            if (arg == NULL) {
                return -EINVAL;
            }
            *(size_t *) arg = (size_t)(memio->buf) + memio->size;
            return 0;
        case IOCTL_SETEND:
            if (arg == NULL) {
                return -EINVAL;
            }
            size_t new_end = *(size_t *) arg;
            size_t buf_start = (size_t) memio->buf;

            if (new_end < buf_start || new_end > buf_start + memio->size) {
                return -EINVAL;
            }

            memio->size = new_end - buf_start;
            return 0;
        default:
            return -ENOTSUP;
    }
}

void seekio_close(struct io * io) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    sio->bkgio->refcnt--;
    ioclose(sio->bkgio);
    kfree(sio);
}

int seekio_cntl(struct io * io, int cmd, void * arg) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long * ullarg = arg;
    int result;

    switch (cmd) {
    case IOCTL_GETBLKSZ:
        return sio->blksz;
    case IOCTL_GETPOS:
        *ullarg = sio->pos;
        return 0;
    case IOCTL_SETPOS:
        // New position must be multiple of block size
        if ((*ullarg & (sio->blksz - 1)) != 0)
            return -EINVAL;
        
        // New position must not be past end
        if (*ullarg > sio->end)
            return -EINVAL;
        
        sio->pos = *ullarg;
        return 0;
    case IOCTL_GETEND:
        *ullarg = sio->end;
        return 0;
    case IOCTL_SETEND:
        // Call backing endpoint ioctl and save result
        result = ioctl(sio->bkgio, IOCTL_SETEND, ullarg);
        if (result == 0)
            sio->end = *ullarg;
        return result;
    default:
        return ioctl(sio->bkgio, cmd, arg);
    }
}

long seekio_read(struct io * io, void * buf, long bufsz) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long const end = sio->end;
    long rcnt;

    // Cannot read past end
    if (end - pos < bufsz)
        bufsz = end - pos;

    if (bufsz == 0)
        return 0;
        
    // Request must be for at least blksz bytes if not zero
    if (bufsz < sio->blksz)
        return -EINVAL;

    // Truncate buffer size to multiple of blksz
    bufsz &= ~(sio->blksz - 1);

    rcnt = ioreadat(sio->bkgio, pos, buf, bufsz);
    sio->pos = pos + ((rcnt < 0) ? 0 : rcnt);
    return rcnt;
}


long seekio_write(struct io * io, const void * buf, long len) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long end = sio->end;
    int result;
    long wcnt;

    if (len == 0)
        return 0;
    
    // Request must be for at least blksz bytes
    if (len < sio->blksz)
        return -EINVAL;
    
    // Truncate length to multiple of blksz
    len &= ~(sio->blksz - 1);

    // Check if write is past end. If it is, we need to change end position.

    if (end - pos < len) {
        if (ULLONG_MAX - pos < len)
            return -EINVAL;
        
        end = pos + len;

        result = ioctl(sio->bkgio, IOCTL_SETEND, &end);
        
        if (result != 0)
            return result;
        
        sio->end = end;
    }

    wcnt = iowriteat(sio->bkgio, sio->pos, buf, len);
    sio->pos = pos + ((wcnt < 0) ? 0 : wcnt);
    return wcnt;
}

long seekio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz)
{
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    return ioreadat(sio->bkgio, pos, buf, bufsz);
}

long seekio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len)
{
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    return iowriteat(sio->bkgio, pos, buf, len);
}

void create_pipe(struct io ** wioptr, struct io ** rioptr) {
    kprintf("[create_pipe] creating pipe\n");
    struct pipe *pipe = kcalloc(1, sizeof(struct pipe));
    if (pipe == NULL) {
        *wioptr = NULL;
        *rioptr = NULL;
        return;
    }
    pipe->buf = alloc_phys_page();
    if (pipe->buf == NULL) {
        kfree(pipe);
        *wioptr = NULL;
        *rioptr = NULL;
        return;
    }
    pipe->refcnt_r = 1;
    pipe->refcnt_w = 1;
    pipe->head = 0;
    pipe->tail = 0;

    lock_init(&pipe->lock);

    condition_init(&pipe->can_read, "can_read");
    condition_init(&pipe->can_write, "can_write");

    ioinit1(&pipe->wio, &pipe_w_intf);
    ioinit1(&pipe->rio, &pipe_r_intf);

    // pipe->wio.refcnt = 1;
    // pipe->rio.refcnt = 1;

    *wioptr = &pipe->wio;
    *rioptr = &pipe->rio;
}

static void pipe_close(struct io *io) {
    if (io == NULL) {
        return;
    }
    struct pipe *pipe;
    int destroyed = 0;
    if (io->intf->read == pipe_read) {
        pipe = (void *)io - offsetof(struct pipe, rio);
    } else {
        pipe = (void *)io - offsetof(struct pipe, wio);
    }
    lock_acquire(&pipe->lock);
    if (io->intf->read == pipe_read) {
        pipe->refcnt_r--;
        condition_broadcast(&pipe->can_write);
    } else {
        pipe->refcnt_w--;
        condition_broadcast(&pipe->can_read);
    }
    // destroy pipe if both read and write ends are closed
    if (pipe->refcnt_r == 0 && pipe->refcnt_w == 0) {
        destroyed = 1;
    }
    lock_release(&pipe->lock);
    if (destroyed) {
        free_phys_page(pipe->buf);
        kfree(pipe);
    }
}
static long pipe_read(struct io *io, void *buf, long bufsz)
{
    if (!io || !buf || bufsz < 0) {
        return -EINVAL;
    }
    struct pipe *pipe = (void *)io - offsetof(struct pipe, rio);
    long nread = 0;

    lock_acquire(&pipe->lock);

    while (nread < bufsz) {
        while (pipe->head == pipe->tail) {
            if (pipe->refcnt_w == 0) {        
                lock_release(&pipe->lock);
                return (nread > 0) ? nread : 0;
            }
            lock_release(&pipe->lock);
            condition_wait(&pipe->can_read);
            lock_acquire(&pipe->lock);
        }
        ((char *)buf)[nread++] = ((char *)pipe->buf)[pipe->head];
        pipe->head = (pipe->head + 1) % PIPE_BUFSZ;
        condition_broadcast(&pipe->can_write);  
    }
    lock_release(&pipe->lock);
    return nread;
}


static long pipe_write(struct io *io, const void *buf, long len)
{
    if (!io || !buf || len < 0)  {
        return -EINVAL;
    }
    struct pipe *pipe = (void *)io - offsetof(struct pipe, wio);
    long nwritten = 0;

    lock_acquire(&pipe->lock);

    while (nwritten < len) {
        while (((pipe->tail + 1) % PIPE_BUFSZ) == pipe->head) {
            if (pipe->refcnt_r == 0) { 
                lock_release(&pipe->lock);
                return -EPIPE;
            }
            lock_release(&pipe->lock);
            condition_wait(&pipe->can_write);
            lock_acquire(&pipe->lock);
        }
        ((char *)pipe->buf)[pipe->tail] = ((const char *)buf)[nwritten++];
        pipe->tail = (pipe->tail + 1) % PIPE_BUFSZ;

        condition_broadcast(&pipe->can_read); 
    }
    lock_release(&pipe->lock);
    return nwritten;
}