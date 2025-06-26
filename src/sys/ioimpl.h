// ioimpl.h - Header for implementors of struct io
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _IOIMPL_H_
#define _IOIMPL_H_

#include "io.h"

// EXPORTED TYPE DEFINITIONS
//

struct io {
    const struct iointf * intf;
    unsigned long refcnt; 
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

// EXPORTED FUNCTION DECLARATIONS
//

// The ioinit0() and ioinit1() functions initialize an I/O endpoint struct
// allocated by the caller. They sets the _intf_ member of the I/O endpoint to
// the _intf_ argument. The ioinit0() function initializes the reference count
// of the I/O endpoint to 0, while the ioinit1() function initializes it to 1.
// The function always succeeds and returns its first argument.

extern struct io * ioinit0(struct io * io, const struct iointf * intf);
extern struct io * ioinit1(struct io * io, const struct iointf * intf);

#endif // _IOIMPL_H_