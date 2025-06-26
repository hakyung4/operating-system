// device.h - Interface to device system
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include "io.h"

// EXPORTED TYPE DEFINITIONS
//

struct device; // opaque decl.

// EXPORTED FUNCTION DECLARATIONS
//

extern char devmgr_initialized;
extern void devmgr_init(void);

// Device catalog

extern int register_device (
    const char * name,
    int (*openfn)(struct io ** ioptr, void * aux),
    void * aux);

extern int open_device (
    const char * name,
    int instno,
    struct io ** ioptr);

// The device_parse_spec function parses a device specification, which is an
// ASCII string identifying a device instance. The specification string must
// consist of one or more non-digit ASCII printable characters representing the
// device name followed by one or more decimal digits representing the instance
// number. If the `spec` points to a well-formed device specification, the
// device_parse_spec function null-terminates the device name in place and
// returns the instance number as a non-negative integer. If `spec` is not
// well-formed, device_parse_spec returns -EINVAL.

extern int parse_device_spec(char * spec);

#endif // _DEVICE_H_