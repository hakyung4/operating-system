// fs.h - File system interface
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _FS_H_
#define _FS_H_

#include "device.h"
#include "io.h"

#define FILE_OPENED (1<<0)

extern char fs_initialized;

extern int fsmount(struct io * io);
extern int fsopen(const char * name, struct io ** ioptr);
extern int fsflush(void);
extern int fscreate(const char * name);
extern int fsdelete(const char * name);

#endif // _FS_H_
