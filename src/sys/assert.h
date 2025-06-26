// assert.h - Assertion statements and panic function
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _ASSERT_H_
#define _ASSERT_H_

#include "console.h" // console_printf
#include "see.h" // halt_failure

extern void __attribute__ ((noreturn)) panic_actual (
    const char * srcfile, int srcline, const char * msg);

extern void __attribute__ ((noreturn)) assert_failed (
    const char * srcfile, int srcline, const char * stmt);

#define assert(c) do { \
    if (!(c)) { \
        assert_failed(__FILE__, __LINE__, #c); \
    } \
} while (0)

#define panic(msg) do { \
    panic_actual(__FILE__, __LINE__, (msg)); \
} while(0)

#endif