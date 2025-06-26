// syscall.h - System call function headers
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include <stddef.h>


extern void __attribute__ ((noreturn)) _exit(void);
extern int _exec(int fd, int argc, char ** argv);
extern int _fork(void);
extern int _wait(int tid);
extern void _print(const char * msg);
extern int _usleep(unsigned long us);
extern int _devopen(int fd, const char * name, int instno);
extern int _fsopen(int fd, const char * name);
extern int _close(int fd);
extern long _read(int fd, void * buf, size_t bufsz);
extern long _write(int fd, const void * buf, size_t len);
extern int _ioctl(int fd, const int cmd, void * arg);
extern int _pipe(int * wfdptr, int * rfdptr);
extern int _iodup(int oldfd, int newfd);
#endif // _SYSCALL_H_