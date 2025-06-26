// console.h - Console i/o
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#include <stddef.h>
#include <stdarg.h>

#include "see.h"

extern char console_initialized;

extern void console_init(void);
extern void kputc(char c);
extern char kgetc(void);
extern void kputs(const char * str);
extern char * kgetsn(char * buf, size_t n);

extern void kprintf(const char * fmt, ...);
extern void kvprintf(const char * fmt, va_list ap);

// The klprintf() is a version of kprintf() that prefixes the printed line with
// a label, source file name, and source line number. It prefixes the printed
// string with "LABEL FILE:LINE: ".
extern void klprintf (
    const char * label,
    const char * flname,
    int lineno,
    const char * fmt, ...);

#ifdef DEBUG
#define debug(...) klprintf("DEBUG", __FILE__, __LINE__, __VA_ARGS__)
#else
#define debug(...) do {} while(0)
#endif

#ifdef TRACE
#define trace(...) klprintf("TRACE", __FILE__, __LINE__, __VA_ARGS__)
#else
#define trace(...) do {} while(0)
#endif

// The following must be defined elsewhere, to be used for console I/O.
// Currently, they are provided in uart.c using the NS8250 UART.
extern void console_device_init(void);
extern void console_device_putc(char c);
extern char console_device_getc(void);

#endif // _CONSOLE_H_