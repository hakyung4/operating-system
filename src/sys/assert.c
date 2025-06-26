// assert.c - Assertion statements and panic function
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "assert.h"
#include "see.h"

// Define a weak kprintf() so that panic() and assert() still work even
// if the kernel is built without console.o.
extern void kprintf(const char * fmt, ...) __attribute__ ((weak));

void panic_actual(const char * srcfile, int srcline, const char * msg) {    
    if (msg != NULL && *msg != '\0')
        klprintf("PANIC", srcfile, srcline, "%s\n", msg);
    else
        klprintf("PANIC", srcfile, srcline, "\n");

    halt_failure();
}

void assert_failed(const char * srcfile, int srcline, const char * stmt) {
    klprintf("ASSERT", srcfile, srcline, "failed (%s)\n", stmt);
    halt_failure();
}

void kprintf(const char * fmt, ...) {
    // nothing
}