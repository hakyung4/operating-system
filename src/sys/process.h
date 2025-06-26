// process.h - User process
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//



#ifndef _PROCESS_H_
#define _PROCESS_H_


#ifndef PROCESS_IOMAX
#define PROCESS_IOMAX 16
#endif

#include "conf.h"
#include "io.h"
#include "thread.h"
#include "trap.h"
#include "memory.h"

// EXPORTED TYPE DEFINITIONS
//


struct process {
    int idx; // index into proctab
    int tid; // thread id of our thread
    mtag_t mtag; // memory space
    struct io * iotab[PROCESS_IOMAX]; // IO objects associated with current process
};

// EXPORTED FUNCTION DECLARATIONS
//


extern char procmgr_initialized;

extern void procmgr_init(void);

extern int process_exec(struct io * exeio, int argc, char ** argv);


extern int process_fork(const struct trap_frame * tfr);
 

extern void __attribute__ ((noreturn)) process_exit(void);



static inline struct process * current_process(void);

// INLINE FUNCTION DEFINITIONS
// 

static inline struct process * current_process(void) {
    return running_thread_process();
}

#endif // _PROCESS_H_