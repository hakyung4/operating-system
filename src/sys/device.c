// device.c - Device manager and device operations
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "device.h"
#include "error.h"
#include "io.h"
#include "conf.h"
#include "heap.h"
#include "string.h"
#include "assert.h"
#include "assert.h"

#include <stddef.h>
#include <limits.h> // INT_MAX

// INTERNAL MACRO DEFINITIONS
//

// The ISPOW2 macro evaluates to 1 if its argument is either zero or a power of
// two. The argument must be an integer type. Cast pointers to `uintptr_t` to
// test pointer alignment.

#define ISPOW2(n) (((n)&((n)-1)) == 0)

// INTERNAL GLOBAL VARIABLES
//

static struct {
    const char * name;
    int (*openfn)(struct io ** ioptr, void * aux);
    void * aux;
} devtab[NDEV];

// EXPORTED GLOBAL VARIABLES
//

char devmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void devmgr_init(void) {
    trace("%s()", __func__);
    devmgr_initialized = 1;
}

int register_device (
    const char * name,
    int (*openfn)(struct io ** ioptr, void * aux),
    void * aux)
{
    int instno = 0;
    int i;

    assert (name != NULL);

    for (i = 0; i < NDEV; i++) {
        if (devtab[i].name == NULL) {
            devtab[i].name = name;
            devtab[i].openfn = openfn;
            devtab[i].aux = aux;
            return instno;
        } else if (strcmp(name, devtab[i].name) == 0)
            instno += 1;
    }
    
    panic(NULL);
}

int open_device(const char * name, int instno, struct io ** ioptr) {
    int i, k = 0;

    trace("%s(%s,%d)", __func__, name, instno);

    // Find numbered instance of device in devtab

    for (i = 0; i < NDEV; i++) {
        if (devtab[i].name == NULL)
            break;

        if (strcmp(name, devtab[i].name) == 0) {
            if (k++ == instno) {
                if (devtab[i].openfn != NULL)
                    return devtab[i].openfn(ioptr, devtab[i].aux);
                else
                    return -ENOTSUP;
            }
        }
    }

    debug("Device %s%d not found", name, instno);
    return -ENODEV;
}

int parse_device_spec(char * spec) {
    char * s = spec; // position in string
    char * p = NULL; // start of number
    unsigned long ulval; // for strtoul
    char * end; // for strtoul

    while (*s != '\0') {
        if ('0' <= *s && *s <= '9') {
            if (p == NULL)
                p = s;
        } else if (' ' < *s && *s < '\x7f')
            p = NULL;
        else
            return -EINVAL;
        
        s += 1;
    }

    if (p != NULL && p != s) {
        ulval = strtoul(p, &end, 10);
        if (ulval <= INT_MAX && *end == '\0') {
            *p = '\0';
            return (int)ulval;
        }
    }

    return -EINVAL;
}