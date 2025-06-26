// io.h - Abstract io interface
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _IO_H_
#define _IO_H_

#include "error.h"
#include "string.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

struct io {
    const struct iointf * intf; // I/O interface for backing endpoint
    unsigned long refcnt; // Number of active references to this I/O endpoint 
};

struct iointf {
    void (*close) (
        struct io * io
    );

    int (*cntl) (
        struct io * io,
        int cmd,
        void * arg
    );

    long (*read) (
        struct io * io,
        void * buf,
        long bufsz
    );

    long (*write) (
        struct io * io,
        const void * buf,
        long len
    );

    long (*readat) (
        struct io * io,
        unsigned long long pos,
        void * buf,
        long len
    );

    long (*writeat) (
        struct io * io,
        unsigned long long pos,
        const void * buf,
        long len
    );
};

struct io_term {
    struct io io; // I/O abstraction
    struct io * rawio; // "raw" I/O object
    int8_t cr_out; // Output CRLF normalization
    int8_t cr_in; // Input CRLF normalization
};


#define IOCTL_GETBLKSZ  0
#define IOCTL_GETEND    2
#define IOCTL_SETEND    3
#define IOCTL_GETPOS    4
#define IOCTL_SETPOS    5

// refcount functions
unsigned long iorefcnt(const struct io * io);
struct io * ioaddref(struct io * io);

// base iopos functions
void ioclose(struct io * io);
long ioread (struct io * io, void * buf, long bufsz);
long iowrite (struct io * io, const void * buf, long len);
long ioreadat (struct io * io, unsigned long long pos, void * buf, long bufsz);
long iowriteat (struct io * io, unsigned long long pos, const void * buf, long len);
int ioctl (struct io * io, int cmd, void * arg);

// put, get, print
static inline int ioputc(struct io * io, char c);
static inline int iogetc(struct io * io);
int ioputs(struct io * io, const char * s);
long ioprintf(struct io * io, const char * fmt, ...);
long iovprintf(struct io * io, const char * fmt, va_list ap);

// I/O term provides three features:
//
//     1. Input CRLF normalization. Any of the following character sequences in
//        the input are converted into a single \n:
//
//            (a) \r\n,
//            (b) \r not followed by \n,
//            (c) \n not preceeded by \r.
//
//     2. Output CRLF normalization. Any \n not preceeded by \r, or \r not
//        followed by \n, is written as \r\n. Sequence \r\n is written as \r\n.
//
//     3. Line editing. The ioterm_getsn function provides line editing of the
//        input.
//
// Input CRLF normalization works by maintaining one bit of state: cr_in.
// Initially cr_in = 0. When a character ch is read from rawio:
// 
// if cr_in = 0 and ch == '\r': return '\n', cr_in <- 1;
// if cr_in = 0 and ch != '\r': return ch;
// if cr_in = 1 and ch == '\r': return \n;
// if cr_in = 1 and ch == '\n': skip, cr_in <- 0;
// if cr_in = 1 and ch != '\r' and ch != '\n': return ch, cr_in <- 0.
//
// Ouput CRLF normalization works by maintaining one bit of state: cr_out.
// Initially, cr_out = 0. When a character ch is written to I/O term:
//
// if cr_out = 0 and ch == '\r': output \r\n to rawio, cr_out <- 1;
// if cr_out = 0 and ch == '\n': output \r\n to rawio;
// if cr_out = 0 and ch != '\r' and ch != '\n': output ch to rawio;
// if cr_out = 1 and ch == '\r': output \r\n to rawio;
// if cr_out = 1 and ch == '\n': no ouput, cr_out <- 0;
// if cr_out = 1 and ch != '\r' and ch != '\n': output ch, cr_out <- 0.


// An io_term object is a wrapper around a "raw" I/O object. It provides newline
// conversion and interactive line-editing for string input.
//
// ioterm_init initializes an io_term object for use with an underlying raw I/O
// object. The /iot/ argument is a pointer to an io_term struct to initialize
// and /rawio/ is a pointer to an io_intf that provides backing I/O.
struct io * ioterm_init(struct io_term * iot, struct io * rawio);


// The ioterm_getsn function reads a line of input of up to /n/ characters from
// a terminal I/O object. The function supports line editing (delete and
// backspace) and limits the input to /n/ characters.
char * ioterm_getsn(struct io_term * iot, char * buf, size_t n);

// definitions for putc and getc
static inline int ioputc(struct io * io, char c) {
    long wlen;

    wlen = iowrite(io, &c, 1);

    if (wlen < 0)
        return wlen;
    else if (wlen == 0)
        return -EIO;
    else
        return (unsigned char)c;
}

static inline int iogetc(struct io * io) {
    long rlen;
    char c;

    rlen = ioread(io, &c, 1);
    
    if (rlen < 0)
        return rlen;
    else if (rlen == 0)
        return -EIO;
    else
        return (unsigned char)c;
}

#endif // _IO_H_