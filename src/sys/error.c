// error.c - Error names
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "error.h"
#include <stddef.h>

extern const char * error_name(int code) {
    static const char * const error_names[] = {
        [0] = "(success)",
        [EINVAL] = "EINVAL",
        [EBUSY] = "EBUSY",
        [ENOTSUP] = "ENOTSUP",
        [ENODEV] = "ENODEV",
        [EIO] = "EIO",
        [EBADFMT] = "EBADFMT",
        [ENOENT] = "ENOENT",
        [EACCESS] = "EACCESS",
        [EBADFD] = "EBADFD",
        [EMFILE] = "EMFILE",
        [EMPROC] = "EMPROC",
        [EMTHR] = "EMTHR",
        [ECHILD] = "ECHILD",
        [ENOMEM] = "ENOMEM",
        [ENODATABLKS] = "ENODATABLKS",
        [ENOINODEBLKS] = "ENOINODEBLKS"
    };

    const char * name;

    if (code < 0)
        code = -code;
    
    if (code < sizeof(error_names)/sizeof(error_names[0]))
        name = error_names[code];
    else
        name = NULL;
    
    return name ? name : "(unknown)";
}
