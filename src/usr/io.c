// io.c - Abstract io interface
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "io.h"

#include <stddef.h>

// INTERNAL TYPE DEFINITIONS
//

struct iovprintf_state {
    struct io * io;
    int err;
};

// INTERNAL FUNCTION DECLARATIONS
//

static void ioterm_close(struct io * io);

static long ioterm_read(struct io * io, void * buf, long len);

static long ioterm_write(struct io * io, const void * buf, long len);

static int ioterm_ioctl(struct io * io, int cmd, void * arg);

static void iovprintf_putc(char c, void * aux);

// EXPORTED FUNCTION DEFINITIONS
//

unsigned long iorefcnt(const struct io * io) {
    return io->refcnt;
}

struct io * ioaddref(struct io * io) {
    io->refcnt += 1;
    return io;
}

void ioclose(struct io * io) {
	if (io->refcnt == 0) {
        return;
    }

    io->refcnt -= 1;

    if (io->refcnt == 0 && io->intf->close != NULL)
        io->intf->close(io);
}

long ioread(struct io * io, void * buf, long bufsz) {
	if (io->intf->read == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->read(io, buf, bufsz);
}

long iowrite(struct io * io, const void * buf, long len) {
	long bufpos = 0; // position in buffer for next write
    long n; // result of last write

    if (io->intf->write == NULL)
        return -ENOTSUP;

    if (len < 0)
        return -EINVAL;
    
    do {
        n = io->intf->write(io, buf, len);

        if (n <= 0)
            return (n < 0) ? n : bufpos;

        bufpos += n;
    } while (bufpos < len);

    return bufpos;
}

long ioreadat (
    struct io * io, unsigned long long pos, void * buf, long bufsz)
{
	if (io->intf->readat == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->readat(io, pos, buf, bufsz);
}

long iowriteat (
    struct io * io, unsigned long long pos, const void * buf, long len)
{
	if (io->intf->writeat == NULL)
        return -ENOTSUP;
    
    if (len < 0)
        return -EINVAL;
    
    return io->intf->writeat(io, pos, buf, len);
}

int ioctl(struct io * io, int cmd, void * arg) {
	if (io->intf->cntl != NULL)
        return io->intf->cntl(io, cmd, arg);
    else if (cmd == IOCTL_GETBLKSZ)
        return 1; // default block size
    else
        return -ENOTSUP;
}

int ioputs(struct io * io, const char * s) {
    const char nl = '\n';
    size_t slen;
    long wlen;

    slen = strlen(s);

    wlen = iowrite(io, s, slen);
    if (wlen < 0)
        return wlen;

    // Write newline

    wlen = iowrite(io, &nl, 1);
    if (wlen < 0)
        return wlen;
    
    return 0;
}

long ioprintf(struct io * io, const char * fmt, ...) {
	va_list ap;
	long result;

	va_start(ap, fmt);
	result = iovprintf(io, fmt, ap);
	va_end(ap);
	return result;
}

long iovprintf(struct io * io, const char * fmt, va_list ap) {
    // state.nout is number of chars written or negative error code
    struct iovprintf_state state = { .io = io, .err = 0 };
    size_t nout;

	nout = vgprintf(iovprintf_putc, &state, fmt, ap);
    return state.err ? state.err : nout;
}

struct io * ioterm_init(struct io_term * iot, struct io * rawio) {
    static const struct iointf ops = {
        .close = ioterm_close,
        .read = ioterm_read,
        .write = ioterm_write,
        .cntl = ioterm_ioctl
    };

    iot->io.intf = &ops;
    iot->rawio = rawio;
    iot->cr_out = 0;
    iot->cr_in = 0;

    return &iot->io;
};

char * ioterm_getsn(struct io_term * iot, char * buf, size_t n) {
    char * p = buf;
    int result;
    char c;

    for (;;) {
        c = iogetc(&iot->io); // already CRLF normalized

        switch (c) {
        case '\133': // escape
            iot->cr_in = 0;
            break;
        case '\r': // should not happen      
        case '\n':
            result = ioputc(iot->rawio, '\r');
            if (result < 0)
                return NULL;
            result = ioputc(iot->rawio, '\n');
            if (result < 0)
                return NULL;
            *p = '\0';
            return buf;
        case '\b': // backspace
        case '\177': // delete
            if (p != buf) {
                p -= 1;
                n += 1;
                
                result = ioputc(iot->rawio, '\b');
                if (result < 0)
                    return NULL;
                result = ioputc(iot->rawio, ' ');
                if (result < 0)
                    return NULL;
                result = ioputc(iot->rawio, '\b');
            } else
                result = ioputc(iot->rawio, '\a'); // beep
            
            if (result < 0)
                return NULL;
            break;

        default:
            if (n > 1) {
                result = ioputc(iot->rawio, c);
                *p++ = c;
                n -= 1;
            } else
                result = ioputc(iot->rawio, '\a'); // beep
            
            if (result < 0)
                return NULL;
        }
    }
}

void ioterm_close(struct io * io) {
    struct io_term * const iot = (void*)io - offsetof(struct io_term, io);
    ioclose(iot->rawio);
}

long ioterm_read(struct io * io, void * buf, long len) {
    struct io_term * const iot = (void*)io - offsetof(struct io_term, io);
    char * rp;
    char * wp;
    long cnt;
    char ch;

    do {
        // Fill buffer using backing io interface

        cnt = ioread(iot->rawio, buf, len);

        if (cnt < 0)
            return cnt;
        
        // Scan though buffer and fix up line endings. We may end up removing some
        // characters from the buffer.  We maintain two pointers /wp/ (write
        // position) and and /rp/ (read position). Initially, rp = wp, however, as
        // we delete characters, /rp/ gets ahead of /wp/, and we copy characters
        // from *rp to *wp to shift the contents of the buffer.
        // 
        // The processing logic is as follows:
        // if cr_in = 0 and ch == '\r': return '\n', cr_in <- 1;
        // if cr_in = 0 and ch != '\r': return ch;
        // if cr_in = 1 and ch == '\r': return \n;
        // if cr_in = 1 and ch == '\n': skip, cr_in <- 0;
        // if cr_in = 1 and ch != '\r' and ch != '\n': return ch, cr_in <- 0.

        wp = rp = buf;
        while ((void*)rp < buf+cnt) {
            ch = *rp++;

            if (iot->cr_in) {
                switch (ch) {
                case '\r':
                    *wp++ = '\n';
                    break;
                case '\n':
                    iot->cr_in = 0;
                    break;
                default:
                    iot->cr_in = 0;
                    *wp++ = ch;
                }
            } else {
                switch (ch) {
                case '\r':
                    iot->cr_in = 1;
                    *wp++ = '\n';
                    break;
                default:
                    *wp++ = ch;
                }
            }
        }

    // We need to return at least one character, however, it is possible that
    // the buffer is still empty. (This would happen if it contained a single
    // '\n' character and cr_in = 1.) If this happens, read more characters.
    } while (wp == buf);

    return (wp - (char*)buf);
}

long ioterm_write(struct io * io, const void * buf, long len) {
    struct io_term * const iot = (void*)io - offsetof(struct io_term, io);
    long acc = 0; // how many bytes from the buffer have been written
    const char * wp;  // everything up to /wp/ in buffer has been written out
    const char * rp;  // position in buffer we're reading
    long cnt;
    char ch;

    // Scan through buffer and look for cases where we need to modify the line
    // ending: lone \r and lone \n get converted to \r\n, while existing \r\n
    // are not modified. We can't modify the buffer, so mwe may need to do
    // partial writes.
    // The strategy we want to implement is:
    // if cr_out = 0 and ch == '\r': output \r\n to rawio, cr_out <- 1;
    // if cr_out = 0 and ch == '\n': output \r\n to rawio;
    // if cr_out = 0 and ch != '\r' and ch != '\n': output ch to rawio;
    // if cr_out = 1 and ch == '\r': output \r\n to rawio;
    // if cr_out = 1 and ch == '\n': no ouput, cr_out <- 0;
    // if cr_out = 1 and ch != '\r' and ch != '\n': output ch, cr_out <- 0.

    wp = rp = buf;

    while ((void*)rp < buf+len) {
        ch = *rp++;
        switch (ch) {
        case '\r':
            // We need to emit a \r\n sequence. If it already occurs in the
            // buffer, we're all set. Otherwise, we need to write what we have
            // from the buffer so far, then write \n, and then continue.
            if ((void*)rp < buf+len && *rp == '\n') {
                // The easy case: buffer already contains \r\n, so keep going.
                iot->cr_out = 0;
                rp += 1;
            } else {
                // Next character is not '\n' or we're at the end of the buffer.
                // We need to write out what we have so far and add a \n.
                cnt = iowrite(iot->rawio, wp, rp - wp);
                if (cnt < 0)
                    return cnt;
                else if (cnt == 0)
                    return acc;
                
                acc += cnt;
                wp += cnt;

                // Now output \n, which does not count toward /acc/.
                cnt = ioputc(iot->rawio, '\n');
                if (cnt < 0)
                    return cnt;
                
                iot->cr_out = 1;
            }
                
            break;
        
        case '\n':
            // If last character was \r, skip the \n. This should only occur at
            // the beginning of the buffer, because we check for a \n after a
            // \r, except if \r is the last character in the buffer. Since we're
            // at the start of the buffer, we don't have to write anything out.
            if (iot->cr_out) {
                iot->cr_out = 0;
                wp += 1;
                break;
            }
            
            // Previous character was not \r, so we need to write a \r first,
            // then the rest of the buffer. But before that, we need to write
            // out what we have so far, up to, but not including the \n we're
            // processing.
            if (wp != rp-1) {
                cnt = iowrite(iot->rawio, wp, rp-1 - wp);
                if (cnt < 0)
                    return cnt;
                else if (cnt == 0)
                    return acc;
                acc += cnt;
                wp += cnt;
            }
            
            cnt = ioputc(iot->rawio, '\r');
            if (cnt < 0)
                return cnt;
            
            // wp should now point to \n. We'll write it when we drain the
            // buffer later.

            iot->cr_out = 0;
            break;
            
        default:
            iot->cr_out = 0;
        }
    }

    if (rp != wp) {
        cnt = iowrite(iot->rawio, wp, rp - wp);

        if (cnt < 0)
            return cnt;
        else if (cnt == 0)
            return acc;
        acc += cnt;
    }

    return acc;
}

int ioterm_ioctl(struct io * io, int cmd, void * arg) {
    struct io_term * const iot = (void*)io - offsetof(struct io_term, io);

    // Pass ioctls through to backing io interface. Seeking is not supported,
    // because we maintain state on the characters output so far.
    if (cmd != IOCTL_SETPOS)
        return ioctl(iot->rawio, cmd, arg);
    else
        return -ENOTSUP;
}

void iovprintf_putc(char c, void * aux) {
    struct iovprintf_state * const state = aux;
    int result;

    if (state->err == 0) {
        result = ioputc(state->io, c);
        if (result < 0)
            state->err = result;
    }
}
