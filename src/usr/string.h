// string.h - String and memory functions
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _STRING_H_
#define _STRING_H_

#include <stddef.h>
#include <stdarg.h>


extern void putc(char c);
extern void dputc(int fd, char c);

extern char getc(void);
extern char dgetc(int fd);

extern void puts(const char * str);
extern void dputs(int fd, const char * str);

extern char * getsn(char * buf, size_t n);
extern void dgetsn(int fd, char * buf, size_t n);

extern void printf(const char * fmt, ...);
extern void dprintf(int fd, const char * fmt, ...);

extern size_t strlen(const char * s);
extern int strcmp(const char * s1, const char * s2);
extern int strncmp(const char * s1, const char * s2, size_t n);
extern char * strncpy(char *dst, const char *src, size_t n);
extern char * strchr(const char * s, int c);
extern char * strrchr(const char * s, int c);

extern void * memset(void * s, int c, size_t n);
extern int memcmp(const void * p1, const void * p2, size_t n);
extern void * memcpy(void * restrict dst, const void * restrict src, size_t n);

extern unsigned long strtoul(const char * str, char ** endptr, int base);
extern size_t snprintf(char * buf, size_t bufsz, const char * fmt, ...);
extern size_t vsnprintf(char * buf, size_t bufsz, const char * fmt, va_list ap);

extern size_t vgprintf (
    void (*putcfn)(char, void*), void * aux,
    const char * fmt, va_list ap);

#endif // _STRING_H_